// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/inference/api/resource_manager.h"

#include <memory>
#include <mutex>
#include <unordered_map>

#include "paddle/fluid/memory/allocation/allocator_facade.h"
#include "paddle/phi/backends/gpu/forwards.h"
#include "paddle/phi/backends/gpu/gpu_decls.h"
#include "paddle/phi/backends/gpu/gpu_info.h"
#include "paddle/phi/backends/gpu/gpu_resources.h"
#include "paddle/phi/common/place.h"
#include "paddle/phi/core/allocator.h"
#include "paddle/phi/core/generator.h"
#include "unsupported/Eigen/CXX11/Tensor"

namespace paddle {
namespace internal {

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
class EigenGpuStreamDevice : public Eigen::StreamInterface {
 public:
  EigenGpuStreamDevice() : scratch_(nullptr), semaphore_(nullptr) {
    Eigen::initializeDeviceProp();
  }
  ~EigenGpuStreamDevice() override {}

  void Reinitialize(gpuStream_t cuda_stream,
                    phi::Allocator* allocator,
                    GPUPlace place) {
    stream_ = cuda_stream;
    allocator_ = allocator;
    device_prop_ = &Eigen::m_deviceProperties[place.device];
  }

  const gpuStream_t& stream() const override { return stream_; }

  const gpuDeviceProp& deviceProperties() const override {
    return *device_prop_;
  }

  void* allocate(size_t num_bytes) const override {
    if (UNLIKELY(num_bytes == 0)) {
      return nullptr;
    }
    auto buf = allocator_->Allocate(num_bytes);
    VLOG(4) << "Eigen allocated at " << buf->ptr() << " requested "
            << num_bytes;
    void* retv = buf->ptr();
    {
      std::lock_guard<std::mutex> lock(mtx_);
      allocations_.emplace(retv, std::move(buf));
    }
    return retv;
  }

  void deallocate(void* buffer) const override {
    if (LIKELY(buffer)) {
      std::lock_guard<std::mutex> lock(mtx_);
      allocations_.erase(buffer);
    }
  }

  void* scratchpad() const override {
    if (scratch_ == NULL) {
      scratch_ = allocate(Eigen::kGpuScratchSize + sizeof(unsigned int));
    }
    return scratch_;
  }

  unsigned int* semaphore() const override {
    if (semaphore_ == NULL) {
      char* scratch = static_cast<char*>(scratchpad()) + Eigen::kGpuScratchSize;
      semaphore_ = reinterpret_cast<unsigned int*>(scratch);
#ifdef PADDLE_WITH_HIP
      PADDLE_ENFORCE_GPU_SUCCESS(
          hipMemsetAsync(semaphore_, 0, sizeof(unsigned int), stream_));
#else
      PADDLE_ENFORCE_GPU_SUCCESS(
          cudaMemsetAsync(semaphore_, 0, sizeof(unsigned int), stream_));
#endif
    }
    return semaphore_;
  }

 private:
  gpuStream_t stream_;                // not owned;
  phi::Allocator* allocator_;         // not owned;
  const gpuDeviceProp* device_prop_;  // not owned;
  mutable void* scratch_;
  mutable unsigned int* semaphore_;
  mutable std::mutex mtx_;  // to protect allocations_
  mutable std::unordered_map<void*, phi::Allocator::AllocationPtr> allocations_;
};
#endif
}  // namespace internal

Eigen::DefaultDevice* CPUContextResource::GetCPUEigenDevice() const {
  return cpu_eigen_device_.get();
}

void CPUContextResource::InitCPUResource() {
  cpu_eigen_device_.reset(new Eigen::DefaultDevice());
}

CPUContextResource::CPUContextResource() { InitCPUResource(); }

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
GPUContextResource::GPUContextResource(const phi::Place& place, void* stream)
    : place_(place) {
  InitGPUResource(stream);
}

GPUContextResource::~GPUContextResource() { DestroyGPUResource(); }

void GPUContextResource::InitGPUResource(void* stream) {
  phi::backends::gpu::GPUDeviceGuard guard(place_.device);
  if (stream == nullptr) {
    owned_stream_ = true;
    phi::InitStream(&stream_);
  } else {
    owned_stream_ = false;
    stream_ = reinterpret_cast<gpuStream_t>(stream);
  }

  InitGpuProperties();
  InitGpuEigenDevice();
  InitDnnHanlde();
  InitBlasHandle();
  InitBlasLtHandle();
  InitSolverHandle();
  InitSparseHandle();
}

void GPUContextResource::DestroyGPUResource() {
  if (owned_stream_) {
#ifdef PADDLE_WITH_HIP
    PADDLE_ENFORCE_GPU_SUCCESS(hipStreamDestroy(stream_));
#else
    PADDLE_ENFORCE_GPU_SUCCESS(cudaStreamDestroy(stream_));
#endif
    stream_ = nullptr;
  }

  DestroyDnnHandle();
  DestroyBlasHandle();
  DestroyBlasLtHandle();
  DestroySolverHandle();
  DestroySparseHandle();
}

void GPUContextResource::InitGpuProperties() {
  phi::InitGpuProperties(place_,
                         &compute_capability_,
                         &runtime_version_,
                         &driver_version_,
                         &multi_process_,
                         &max_threads_per_mp_,
                         &max_threads_per_block_,
                         &max_grid_dim_size_);
}

void GPUContextResource::InitGpuEigenDevice() {
  auto* allocator = paddle::memory::allocation::AllocatorFacade::Instance()
                        .GetAllocator(place_)
                        .get();
  eigen_stream_.reset(new internal::EigenGpuStreamDevice());
  eigen_stream_->Reinitialize(stream_, allocator, place_);
  gpu_eigen_device_.reset(new Eigen::GpuDevice(eigen_stream_.get()));
}

void GPUContextResource::InitDnnHanlde() {
  phi::InitDnnHandle(&dnn_handle_, stream_, place_);
}

void GPUContextResource::DestroyDnnHandle() {
  phi::DestroyDnnHandle(dnn_handle_);
}

void GPUContextResource::InitBlasHandle() {
  phi::InitBlasHandle(&blas_handle_, stream_);
#ifdef PADDLE_WITH_CUDA
#if CUDA_VERSION >= 9000
  phi::InitBlasHandle(&blas_tensor_core_handle_, stream_);
  PADDLE_RETRY_CUDA_SUCCESS(phi::dynload::cublasSetMathMode(
      blas_tensor_core_handle_, CUBLAS_TENSOR_OP_MATH));
#endif
#if CUDA_VERSION >= 11000
  phi::InitBlasHandle(&blas_tf32_tensor_core_handle_, stream_);
  PADDLE_RETRY_CUDA_SUCCESS(phi::dynload::cublasSetMathMode(
      blas_tf32_tensor_core_handle_, CUBLAS_TF32_TENSOR_OP_MATH));
#endif
#endif
}

void GPUContextResource::DestroyBlasHandle() {
  phi::DestroyBlasHandle(blas_handle_);
  phi::DestroyBlasHandle(blas_tensor_core_handle_);
  phi::DestroyBlasHandle(blas_tf32_tensor_core_handle_);
}

void GPUContextResource::InitBlasLtHandle() {
  phi::InitBlasLtHandle(&blaslt_handle_);
}

void GPUContextResource::DestroyBlasLtHandle() {
  phi::DestroyBlasLtHandle(blaslt_handle_);
}

void GPUContextResource::InitSolverHandle() {
  phi::InitSolverHandle(&solver_handle_, stream_);
}

void GPUContextResource::DestroySolverHandle() {
  phi::DestroySolverHandle(solver_handle_);
}

void GPUContextResource::InitSparseHandle() {
  phi::InitSparseHandle(&sparse_handle_, stream_);
}

void GPUContextResource::DestroySparseHandle() {
  phi::DestroySparseHandle(sparse_handle_);
}

gpuStream_t GPUContextResource::GetStream() const { return stream_; }

dnnHandle_t GPUContextResource::GetDnnHandle() const { return dnn_handle_; }

blasHandle_t GPUContextResource::GetBlasHandle() const { return blas_handle_; }

blasHandle_t GPUContextResource::GetBlasTensorCoreHandle() const {
  return blas_tensor_core_handle_;
}

blasHandle_t GPUContextResource::GetBlasTF32Handle() const {
  return blas_tf32_tensor_core_handle_;
}

blasLtHandle_t GPUContextResource::GetBlasLtHandle() const {
  return blaslt_handle_;
}

phi::solverHandle_t GPUContextResource::GetSolverDnHandle() const {
  return solver_handle_;
}

phi::sparseHandle_t GPUContextResource::GetSparseHandle() const {
  return sparse_handle_;
}

Eigen::GpuDevice* GPUContextResource::GetGpuEigenDevice() const {
  return gpu_eigen_device_.get();
}

int GPUContextResource::GetGpuComputeCapability() const {
  return compute_capability_;
}

int GPUContextResource::GetGpuRuntimeVersion() const {
  return runtime_version_;
}

int GPUContextResource::GetGpuDriverVersion() const { return driver_version_; }

int GPUContextResource::GetGPUMultiProcessors() const { return multi_process_; }

int GPUContextResource::GetGpuMaxThreadsPerMp() const {
  return max_threads_per_mp_;
}

int GPUContextResource::GetGpuMaxThreadsPerBlock() const {
  return max_threads_per_block_;
}

std::array<int, 3> GPUContextResource::GetGpuMaxGridDimSize() const {
  return max_grid_dim_size_;
}

#endif

void ResourceManager::InitCPUResource() {
  std::lock_guard<std::mutex> lock_gurad(cpu_mutex_);
  if (cpu_resource_ == nullptr) {
    cpu_resource_.reset(new CPUContextResource());
  }
}

CPUContextResource* ResourceManager::GetCPUResource() const {
  PADDLE_ENFORCE_NOT_NULL(
      cpu_resource_.get(),
      platform::errors::PreconditionNotMet("cpu_resource should be not null!"));
  return cpu_resource_.get();
}

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
void* ResourceManager::InitGPUResource(const phi::Place& place, void* stream) {
  std::lock_guard<std::mutex> lock_gurad(gpu_mutex_);
  if (gpu_resources_.count(stream)) {
    Increase(stream);
    return stream;
  } else {
    std::unique_ptr<GPUContextResource> resource{
        new GPUContextResource(place, stream)};
    gpuStream_t s = resource->GetStream();
    ref_count_[s] = 1;
    gpu_resources_.emplace(s, std::move(resource));
    return s;
  }
}

void ResourceManager::DestroyGPUResource(void* stream) {
  PADDLE_ENFORCE_EQ(gpu_resources_.count(stream),
                    true,
                    platform::errors::InvalidArgument(
                        "The stream[%p] not found in gpu_resources.", stream));
  Decrease(stream);
}

void ResourceManager::Decrease(void* stream) {
  PADDLE_ENFORCE_EQ(ref_count_.count(stream),
                    true,
                    platform::errors::InvalidArgument(
                        "The stream[%p] not found in ref_count.", stream));
  --ref_count_[stream];
  if (ref_count_[stream] == 0) {
    ref_count_.erase(stream);
    gpu_resources_.erase(stream);
  }
}

void ResourceManager::Increase(void* stream) {
  PADDLE_ENFORCE_EQ(ref_count_.count(stream),
                    true,
                    platform::errors::InvalidArgument(
                        "The stream[%p] not found in ref_count.", stream));
  ++ref_count_[stream];
}

GPUContextResource* ResourceManager::GetGPUResource(void* stream) const {
  PADDLE_ENFORCE_EQ(gpu_resources_.count(stream),
                    true,
                    platform::errors::InvalidArgument(
                        "The stream[%p] not found in gpu_resources.", stream));
  return gpu_resources_.at(stream).get();
}

int ResourceManager::RefCount(void* stream) const {
  if (ref_count_.count(stream) == 0) return 0;
  return ref_count_.at(stream);
}
#endif

}  // namespace paddle
