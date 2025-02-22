# Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import unittest
import os
import json

import paddle
import numpy as np
import paddle.nn as nn
import paddle.utils as utils
import paddle.static as static
import paddle.nn.functional as F

from paddle.distributed import fleet
import paddle.distributed.auto_parallel as auto
from paddle.distributed.auto_parallel.dist_context import DistributedContext
from paddle.distributed.auto_parallel.utils import print_program_with_dist_attr

paddle.enable_static()

batch_size = 4
hidden_size = 1024
sequence_len = 512
_g_process_mesh = [[0, 1], [2, 3]]


def get_random_inputs_and_labels(input_shape, label_shape):
    input = np.random.random(size=input_shape).astype('float32')
    label = np.random.random(size=label_shape).astype('float32')
    return input, label


def batch_generator_creator():

    def __reader__():
        for _ in range(batch_size):
            batch_input, batch_label = get_random_inputs_and_labels(
                [batch_size, sequence_len, hidden_size],
                [batch_size, sequence_len, 1])
            yield batch_input, batch_label

    return __reader__


class MLPLayer(nn.Layer):

    def __init__(self,
                 hidden_size=1024,
                 intermediate_size=4 * 1024,
                 dropout_ratio=0.1,
                 initializer_range=0.02):
        super(MLPLayer, self).__init__()
        d_model = hidden_size
        dim_feedforward = intermediate_size
        param_initializer = nn.initializer.Normal(mean=0.0,
                                                  std=initializer_range)

        self.norm = nn.LayerNorm(d_model, epsilon=1e-5)
        self.linear0 = nn.Linear(
            d_model,
            dim_feedforward,
            weight_attr=paddle.ParamAttr(initializer=param_initializer),
            bias_attr=None)
        self.linear1 = nn.Linear(
            dim_feedforward,
            d_model,
            weight_attr=paddle.ParamAttr(initializer=param_initializer),
            bias_attr=None)

    def forward(self, input):
        out = self.norm(input)
        auto.shard_tensor(self.linear0.weight,
                          dist_attr={
                              "process_mesh": _g_process_mesh[0],
                              "dims_mapping": [-1, 0]
                          })
        out = self.linear0(out)
        out = F.gelu(out, approximate=True)
        auto.shard_tensor(self.linear1.weight,
                          dist_attr={
                              "process_mesh": _g_process_mesh[1],
                              "dims_mapping": [0, -1]
                          })
        out = self.linear1(out)

        return out


def get_program():
    dist_strategy = fleet.DistributedStrategy()
    dist_strategy.semi_auto = True
    # fleet.init(is_collective=True, strategy=dist_strategy)

    train_program = static.Program()
    start_program = static.Program()
    with static.program_guard(train_program, start_program):
        # input
        input = static.data(name="input",
                            shape=[batch_size, sequence_len, hidden_size],
                            dtype='float32')
        label = static.data(name="label",
                            shape=[batch_size, sequence_len, 1],
                            dtype='float32')
        data_holder = [input, label]
        # dataloader
        dataloader = paddle.io.DataLoader.from_generator(feed_list=data_holder,
                                                         capacity=4 *
                                                         batch_size,
                                                         iterable=False)
        dataloader.set_batch_generator(batch_generator_creator(),
                                       places=paddle.static.cuda_places())
        # data dist_attr
        auto.shard_tensor(input,
                          dist_attr={
                              "process_mesh": _g_process_mesh[0],
                              "dims_mapping": [0, -1, -1]
                          })
        auto.shard_tensor(label,
                          dist_attr={
                              "process_mesh": _g_process_mesh[0],
                              "dims_mapping": [0, -1, -1]
                          })

        mlp_start = MLPLayer(hidden_size=hidden_size,
                             intermediate_size=4 * hidden_size,
                             dropout_ratio=0.1,
                             initializer_range=0.02)
        pred = mlp_start(input)

        mlp_mid = MLPLayer(hidden_size=hidden_size,
                           intermediate_size=4 * hidden_size,
                           dropout_ratio=0.1,
                           initializer_range=0.02)
        pred = mlp_mid(pred)

        mlp_end = MLPLayer(hidden_size=hidden_size,
                           intermediate_size=4 * hidden_size,
                           dropout_ratio=0.1,
                           initializer_range=0.02)
        pred = mlp_end(pred)

        error_cost = paddle.nn.functional.square_error_cost(pred, label)
        loss = paddle.mean(error_cost)

        optimizer = paddle.optimizer.Adam(learning_rate=0.00001,
                                          beta1=0.9,
                                          beta2=0.999,
                                          epsilon=1e-08,
                                          grad_clip=None)

        feed_vars = {"inputs": [input], "labels": [label]}
        fetch_vars = {"loss": [loss]}

    return train_program, start_program, dataloader, loss, optimizer, feed_vars, fetch_vars


class TestDistributedContext(unittest.TestCase):

    def test_backup_restore(self):
        train_program, start_program, dataloader, loss, optimizer, feed_vars, fetch_vars = get_program(
        )
        dist_context = DistributedContext(train_program, start_program,
                                          optimizer, loss, feed_vars,
                                          fetch_vars)
        dist_context.initialize()

        dist_context._backup(serial=True, dist=True)
        dist_context._restore(serial=True,
                              serial_mode="to_backup",
                              dist=True,
                              dist_mode="to_backup")

        dist_context._backup(serial=True, dist=True)
        dist_context._restore(serial=True,
                              serial_mode="to_original",
                              dist=True,
                              dist_mode="to_original")

        dist_context._backup(serial=True, dist=True)
        dist_context._restore(serial=True, dist=True, dist_mode="to_default")

        dist_context._backup(serial=True, dist=True)
        dist_context._restore(serial=True, dist=True, dist_mode="to_nothing")


if __name__ == "__main__":
    unittest.main()
