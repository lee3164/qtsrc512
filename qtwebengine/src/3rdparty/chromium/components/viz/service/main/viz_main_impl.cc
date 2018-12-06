// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/main/viz_main_impl.h"

#include <memory>
#include <utility>

#include "base/command_line.h"
#include "base/message_loop/message_loop.h"
#include "base/power_monitor/power_monitor_device_source.h"
#include "base/single_thread_task_runner.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/trace_event/memory_dump_manager.h"
#include "build/build_config.h"
#include "components/viz/common/switches.h"
#include "components/viz/service/display_embedder/gpu_display_provider.h"
#include "components/viz/service/display_embedder/in_process_gpu_memory_buffer_manager.h"
#include "components/viz/service/display_embedder/server_shared_bitmap_manager.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/service/gl/gpu_service_impl.h"
#include "gpu/command_buffer/common/activity_flags.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/config/gpu_switches.h"
#include "gpu/ipc/common/gpu_memory_buffer_support.h"
#include "gpu/ipc/gpu_in_process_thread_service.h"
#include "gpu/ipc/service/gpu_memory_buffer_factory.h"
#include "gpu/ipc/service/gpu_watchdog_thread.h"
#include "media/gpu/buildflags.h"
#include "services/metrics/public/cpp/delegating_ukm_recorder.h"
#include "services/metrics/public/cpp/mojo_ukm_recorder.h"
#include "services/metrics/public/mojom/constants.mojom.h"
#include "services/service_manager/public/cpp/connector.h"
#include "third_party/skia/include/core/SkFontLCDConfig.h"
#include "ui/gfx/switches.h"

#if defined(OS_CHROMEOS) && BUILDFLAG(USE_VAAPI)
#include "media/gpu/vaapi/vaapi_wrapper.h"
#endif

#if defined(USE_OZONE)
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/ozone_switches.h"
#endif

namespace {

std::unique_ptr<viz::CompositorThreadType> CreateAndStartCompositorThread() {
  const char* thread_name = "VizCompositorThread";

#if defined(OS_ANDROID)
  auto thread = std::make_unique<base::android::JavaHandlerThread>(
      thread_name, base::ThreadPriority::DISPLAY);
  thread->Start();
  return thread;
#else
  auto thread = std::make_unique<base::Thread>(thread_name);
  base::Thread::Options thread_options;
#if defined(OS_WIN)
  // Windows needs a UI message loop for child HWND. Other platforms can use the
  // default message loop type.
  thread_options.message_loop_type = base::MessageLoop::TYPE_UI;
#endif

#if defined(OS_CHROMEOS)
  thread_options.priority = base::ThreadPriority::DISPLAY;
#endif
  CHECK(thread->StartWithOptions(thread_options));
  return thread;
#endif
}

std::unique_ptr<base::Thread> CreateAndStartIOThread() {
  // TODO(sad): We do not need the IO thread once gpu has a separate process.
  // It should be possible to use |main_task_runner_| for doing IO tasks.
  base::Thread::Options thread_options(base::MessageLoop::TYPE_IO, 0);
  thread_options.priority = base::ThreadPriority::NORMAL;
#if defined(OS_ANDROID) || defined(OS_CHROMEOS)
  // TODO(reveman): Remove this in favor of setting it explicitly for each
  // type of process.
  thread_options.priority = base::ThreadPriority::DISPLAY;
#endif
  auto io_thread = std::make_unique<base::Thread>("GpuIOThread");
  CHECK(io_thread->StartWithOptions(thread_options));
  return io_thread;
}

}  // namespace

namespace viz {

VizMainImpl::ExternalDependencies::ExternalDependencies() = default;

VizMainImpl::ExternalDependencies::~ExternalDependencies() = default;

VizMainImpl::ExternalDependencies::ExternalDependencies(
    ExternalDependencies&& other) = default;

VizMainImpl::ExternalDependencies& VizMainImpl::ExternalDependencies::operator=(
    ExternalDependencies&& other) = default;

VizMainImpl::VizMainImpl(Delegate* delegate,
                         ExternalDependencies dependencies,
                         std::unique_ptr<gpu::GpuInit> gpu_init)
    : delegate_(delegate),
      dependencies_(std::move(dependencies)),
      gpu_init_(std::move(gpu_init)),
      gpu_thread_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      binding_(this),
      associated_binding_(this) {
  // TODO(crbug.com/609317): Remove this when Mus Window Server and GPU are
  // split into separate processes. Until then this is necessary to be able to
  // run Mushrome (chrome with mus) with Mus running in the browser process.
  if (!base::PowerMonitor::Get()) {
    power_monitor_ = std::make_unique<base::PowerMonitor>(
        std::make_unique<base::PowerMonitorDeviceSource>());
  }

  if (!gpu_init_) {
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    gpu::GpuPreferences gpu_preferences;
    if (command_line->HasSwitch(switches::kGpuPreferences)) {
      std::string value =
          command_line->GetSwitchValueASCII(switches::kGpuPreferences);
      bool success = gpu_preferences.FromSwitchValue(value);
      CHECK(success);
    }
#if defined(OS_CHROMEOS) && BUILDFLAG(USE_VAAPI)
    // Initialize media codec. The UI service is running in a priviliged
    // process. We don't need care when to initialize media codec. When we have
    // a separate GPU (or VIZ) service process, we should initialize them
    // before sandboxing
    media::VaapiWrapper::PreSandboxInitialization();
#endif

    // Initialize GpuInit before starting the IO or compositor threads.
    gpu_init_ = std::make_unique<gpu::GpuInit>();
    gpu_init_->set_sandbox_helper(this);

#if defined(USE_OZONE)
    command_line->AppendSwitch(switches::kEnableDrmMojo);
#endif

    // TODO(crbug.com/609317): Use InitializeAndStartSandbox() when gpu-mus is
    // split into a separate process.
    gpu_init_->InitializeInProcess(command_line, gpu_preferences);
  }

  if (!dependencies_.io_thread_task_runner)
    io_thread_ = CreateAndStartIOThread();
  if (dependencies_.create_display_compositor) {
    compositor_thread_ = CreateAndStartCompositorThread();
    compositor_thread_task_runner_ = compositor_thread_->task_runner();
    if (delegate_)
      delegate_->PostCompositorThreadCreated(
          compositor_thread_task_runner_.get());
  }

  CreateUkmRecorderIfNeeded(dependencies.connector);

  // We need to provide GpuServiceImpl a callback to exit the GPU process. With
  // OOP-D this requires destroying RootCompositorFrameSinkImpls on the
  // compositor thread while the GPU thread is still running to avoid deadlock.
  // For non OOP-D we can simply quit the GPU thread RunLoop.
  base::OnceClosure exit_callback =
      compositor_thread_
          ? base::BindOnce(&VizMainImpl::ExitProcess, base::Unretained(this))
          : base::BindOnce(&base::RunLoop::QuitCurrentDeprecated);
  gpu_service_ = std::make_unique<GpuServiceImpl>(
      gpu_init_->gpu_info(), gpu_init_->TakeWatchdogThread(), io_task_runner(),
      gpu_init_->gpu_feature_info(), gpu_init_->gpu_preferences(),
      gpu_init_->gpu_info_for_hardware_gpu(),
      gpu_init_->gpu_feature_info_for_hardware_gpu(),
      gpu_init_->vulkan_implementation(), std::move(exit_callback));
  if (dependencies_.create_display_compositor)
    gpu_service_->set_oopd_enabled();
}

VizMainImpl::~VizMainImpl() {
  DCHECK(gpu_thread_task_runner_->BelongsToCurrentThread());

  // The compositor holds on to some resources from gpu service. So destroy the
  // compositor first, before destroying the gpu service. However, before the
  // compositor is destroyed, close the binding, so that the gpu service doesn't
  // need to process commands from the host as it is shutting down.
  binding_.Close();
  associated_binding_.Close();

  if (compositor_thread_) {
    // Destroy all objects owned on the compositor thread before shutting down
    // the thread. All RootCompositorFrameSinks must be destroyed before now,
    // otherwise the compositor thread will deadlock waiting for a response from
    // the blocked GPU thread.
    compositor_thread_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&VizMainImpl::TearDownOnCompositorThread,
                                  base::Unretained(this)));
    compositor_thread_->Stop();
    compositor_thread_.reset();
    compositor_thread_task_runner_.reset();
  }

  if (ukm_recorder_)
    ukm::DelegatingUkmRecorder::Get()->RemoveDelegate(ukm_recorder_.get());
}

void VizMainImpl::SetLogMessagesForHost(LogMessages log_messages) {
  log_messages_ = std::move(log_messages);
}

void VizMainImpl::Bind(mojom::VizMainRequest request) {
  binding_.Bind(std::move(request));
}

void VizMainImpl::BindAssociated(mojom::VizMainAssociatedRequest request) {
  associated_binding_.Bind(std::move(request));
}

void VizMainImpl::CreateGpuService(
    mojom::GpuServiceRequest request,
    mojom::GpuHostPtr gpu_host,
    discardable_memory::mojom::DiscardableSharedMemoryManagerPtr
        discardable_memory_manager,
    mojo::ScopedSharedBufferHandle activity_flags,
    gfx::FontRenderParams::SubpixelRendering subpixel_rendering) {
  DCHECK(gpu_thread_task_runner_->BelongsToCurrentThread());

  // If GL is disabled then don't try to collect GPUInfo, we're not using GPU.
  if (gl::GetGLImplementation() != gl::kGLImplementationDisabled)
    gpu_service_->UpdateGPUInfo();

  for (const LogMessage& log : log_messages_)
    gpu_host->RecordLogMessage(log.severity, log.header, log.message);
  log_messages_.clear();
  if (!gpu_init_->init_successful()) {
    LOG(ERROR) << "Exiting GPU process due to errors during initialization";
    gpu_service_.reset();
    gpu_host->DidFailInitialize();
    if (delegate_)
      delegate_->OnInitializationFailed();
    return;
  }

  if (!gpu_init_->gpu_info().in_process_gpu) {
    // If the GPU is running in the browser process, discardable memory manager
    // has already been initialized.
    discardable_shared_memory_manager_ = std::make_unique<
        discardable_memory::ClientDiscardableSharedMemoryManager>(
        std::move(discardable_memory_manager), io_task_runner());
    base::DiscardableMemoryAllocator::SetInstance(
        discardable_shared_memory_manager_.get());
  }

  SkFontLCDConfig::SetSubpixelOrder(
      gfx::FontRenderParams::SubpixelRenderingToSkiaLCDOrder(
          subpixel_rendering));
  SkFontLCDConfig::SetSubpixelOrientation(
      gfx::FontRenderParams::SubpixelRenderingToSkiaLCDOrientation(
          subpixel_rendering));

  gpu_service_->Bind(std::move(request));
  gpu_service_->InitializeWithHost(
      std::move(gpu_host),
      gpu::GpuProcessActivityFlags(std::move(activity_flags)),
      dependencies_.sync_point_manager, dependencies_.shutdown_event);

  if (!pending_frame_sink_manager_params_.is_null()) {
    CreateFrameSinkManagerInternal(
        std::move(pending_frame_sink_manager_params_));
    pending_frame_sink_manager_params_.reset();
  }
  if (delegate_)
    delegate_->OnGpuServiceConnection(gpu_service_.get());
}

void VizMainImpl::CreateUkmRecorderIfNeeded(
    service_manager::Connector* connector) {
  // If GPU is running in the browser process, we can use browser's UKMRecorder.
  if (gpu_init_->gpu_info().in_process_gpu)
    return;

  DCHECK(connector) << "Unable to initialize UKMRecorder in the GPU process - "
                    << "no valid connector.";
  ukm_recorder_ = ukm::MojoUkmRecorder::Create(connector);
  ukm::DelegatingUkmRecorder::Get()->AddDelegate(ukm_recorder_->GetWeakPtr());
}

void VizMainImpl::CreateFrameSinkManager(
    mojom::FrameSinkManagerParamsPtr params) {
  DCHECK(compositor_thread_task_runner_);
  DCHECK(gpu_thread_task_runner_->BelongsToCurrentThread());
  if (!gpu_service_ || !gpu_service_->is_initialized()) {
    DCHECK(pending_frame_sink_manager_params_.is_null());
    pending_frame_sink_manager_params_ = std::move(params);
    return;
  }
  CreateFrameSinkManagerInternal(std::move(params));
}

void VizMainImpl::CreateFrameSinkManagerInternal(
    mojom::FrameSinkManagerParamsPtr params) {
  DCHECK(!task_executor_);
  DCHECK(gpu_service_);
  DCHECK(gpu_thread_task_runner_->BelongsToCurrentThread());

  task_executor_ = base::MakeRefCounted<gpu::GpuInProcessThreadService>(
      gpu_thread_task_runner_, gpu_service_->sync_point_manager(),
      gpu_service_->mailbox_manager(), gpu_service_->share_group(),
      gpu_service_->gpu_feature_info(),
      gpu_service_->gpu_channel_manager()->gpu_preferences());

  compositor_thread_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&VizMainImpl::CreateFrameSinkManagerOnCompositorThread,
                     base::Unretained(this), std::move(params)));
}

void VizMainImpl::CreateFrameSinkManagerOnCompositorThread(
    mojom::FrameSinkManagerParamsPtr params) {
  DCHECK(compositor_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!frame_sink_manager_);

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  server_shared_bitmap_manager_ = std::make_unique<ServerSharedBitmapManager>();
  base::trace_event::MemoryDumpManager::GetInstance()->RegisterDumpProvider(
      server_shared_bitmap_manager_.get(), "viz::ServerSharedBitmapManager",
      base::ThreadTaskRunnerHandle::Get());

  auto* gpu_channel_manager = gpu_service_->gpu_channel_manager();
  gpu::ImageFactory* image_factory = nullptr;
  if (gpu_channel_manager->gpu_memory_buffer_factory()) {
    image_factory =
        gpu_channel_manager->gpu_memory_buffer_factory()->AsImageFactory();
  }
  display_provider_ = std::make_unique<GpuDisplayProvider>(
      params->restart_id, gpu_service_.get(), task_executor_,
      gpu_channel_manager->delegate(),
      std::make_unique<InProcessGpuMemoryBufferManager>(gpu_channel_manager),
      image_factory, server_shared_bitmap_manager_.get(),
      command_line->HasSwitch(switches::kHeadless),
      command_line->HasSwitch(switches::kRunAllCompositorStagesBeforeDraw));

  mojom::FrameSinkManagerClientPtr client(
      std::move(params->frame_sink_manager_client));
  base::Optional<uint32_t> activation_deadline_in_frames;
  if (params->use_activation_deadline)
    activation_deadline_in_frames = params->activation_deadline_in_frames;
  frame_sink_manager_ = std::make_unique<FrameSinkManagerImpl>(
      server_shared_bitmap_manager_.get(), activation_deadline_in_frames,
      display_provider_.get());
  frame_sink_manager_->BindAndSetClient(std::move(params->frame_sink_manager),
                                        nullptr, std::move(client));
}

void VizMainImpl::TearDownOnCompositorThread() {
  DCHECK(compositor_thread_task_runner_->BelongsToCurrentThread());

  base::trace_event::MemoryDumpManager::GetInstance()->UnregisterDumpProvider(
      server_shared_bitmap_manager_.get());

  frame_sink_manager_.reset();
  display_provider_.reset();
  server_shared_bitmap_manager_.reset();
}

void VizMainImpl::CleanupForShutdownOnCompositorThread() {
  DCHECK(compositor_thread_task_runner_->BelongsToCurrentThread());

  if (frame_sink_manager_)
    frame_sink_manager_->ForceShutdown();
}

void VizMainImpl::ExitProcess() {
  DCHECK(gpu_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(compositor_thread_task_runner_);

  // Close mojom::VizMain bindings first so the browser can't try to reconnect.
  binding_.Close();
  associated_binding_.Close();

  // PostTask to the compositor thread to cleanup and then exit the GPU thread
  // RunLoop once that is finished. Unretained is safe here because |this| owns
  // |compositor_thread_|.
  compositor_thread_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&VizMainImpl::CleanupForShutdownOnCompositorThread,
                     base::Unretained(this)),
      base::BindOnce([]() { base::RunLoop::QuitCurrentDeprecated(); }));
}

void VizMainImpl::PreSandboxStartup() {
  // TODO(sad): https://crbug.com/645602
}

bool VizMainImpl::EnsureSandboxInitialized(
    gpu::GpuWatchdogThread* watchdog_thread,
    const gpu::GPUInfo* gpu_info,
    const gpu::GpuPreferences& gpu_prefs) {
  // TODO(sad): https://crbug.com/645602
  return true;
}

}  // namespace viz