/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_decoder.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/emulator.h"
#include "xenia/kernel/objects/xthread.h"
#include "xenia/profiling.h"

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame/subframe/etc:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace xe {
namespace apu {

using namespace xe::cpu;

// Size of a hardware XMA context.
const uint32_t kXmaContextSize = 64;
// Total number of XMA contexts available.
const uint32_t kXmaContextCount = 320;

AudioSystem::AudioSystem(Emulator* emulator)
    : emulator_(emulator), memory_(emulator->memory()), worker_running_(false),
    decoder_running_(false) {
  memset(clients_, 0, sizeof(clients_));
  for (size_t i = 0; i < maximum_client_count_; ++i) {
    unused_clients_.push(i);
  }
  for (size_t i = 0; i < xe::countof(client_wait_handles_); ++i) {
    client_wait_handles_[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
  }
}

AudioSystem::~AudioSystem() {
  for (size_t i = 0; i < xe::countof(client_wait_handles_); ++i) {
    CloseHandle(client_wait_handles_[i]);
  }
}

X_STATUS AudioSystem::Setup() {
  processor_ = emulator_->processor();

  // Let the processor know we want register access callbacks.
  emulator_->memory()->AddVirtualMappedRange(
      0x7FEA0000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<MMIOWriteCallback>(MMIOWriteRegisterThunk));

  // Setup XMA contexts ptr.
  registers_.xma_context_array_ptr = memory()->SystemHeapAlloc(
      kXmaContextSize * kXmaContextCount, 256, kSystemHeapPhysical);
  // Add all contexts to the free list.
  for (int i = kXmaContextCount - 1; i >= 0; --i) {
    uint32_t ptr = registers_.xma_context_array_ptr + i * kXmaContextSize;

    // Initialize it
    xma_context_array_[i].guest_ptr = ptr;
    xma_context_array_[i].in_use = false;

    // Create a new decoder per context
    // Needed because some data needs to be persisted across calls
    // TODO: Need to destroy this on class destruction
    xma_context_array_[i].decoder = new AudioDecoder();
    xma_context_array_[i].decoder->Initialize(16);
  }
  registers_.next_context = 1;

  // Threads

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          emulator()->kernel_state(), 128 * 1024, 0, [this]() {
            this->WorkerThreadMain();
            return 0;
          }));
  worker_thread_->Create();

  decoder_running_ = true;
  decoder_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          emulator()->kernel_state(), 128 * 1024, 0, [this]() {
            DecoderThreadMain();
            return 0;
          }));
  decoder_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  xe::threading::set_name("Audio Worker");

  // Initialize driver and ringbuffer.
  Initialize();

  auto processor = emulator_->processor();

  // Main run loop.
  while (worker_running_) {
    auto result =
        WaitForMultipleObjectsEx(DWORD(xe::countof(client_wait_handles_)),
                                 client_wait_handles_, FALSE, INFINITE, FALSE);
    if (result == WAIT_FAILED ||
        result == WAIT_OBJECT_0 + maximum_client_count_) {
      continue;
    }

    size_t pumped = 0;
    if (result >= WAIT_OBJECT_0 &&
        result <= WAIT_OBJECT_0 + (maximum_client_count_ - 1)) {
      size_t index = result - WAIT_OBJECT_0;
      do {
        lock_.lock();
        uint32_t client_callback = clients_[index].callback;
        uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
        lock_.unlock();

        if (client_callback) {
          uint64_t args[] = {client_callback_arg};
          processor->Execute(worker_thread_->thread_state(), client_callback,
                             args, xe::countof(args));
        }
        pumped++;
        index++;
      } while (index < maximum_client_count_ &&
               WaitForSingleObject(client_wait_handles_[index], 0) ==
                   WAIT_OBJECT_0);
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      Sleep(500);
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

void AudioSystem::DecoderThreadMain() {
  xe::threading::set_name("Audio Decoder");

  while (decoder_running_) {
    // Wait for the fence
    // FIXME: This actually does nothing once signaled once
    decoder_fence_.Wait();

    // Check to see if we're supposed to exit
    if (!decoder_running_) {
      break;
    }

    // Okay, let's loop through XMA contexts to find ones we need to decode!
    for (uint32_t n = 0; n < kXmaContextCount; n++) {
      XMAContext& context = xma_context_array_[n];
      if (!context.lock.try_lock()) {
        // Someone else has the lock.
        continue;
      }

      // Skip unused contexts
      if (!context.in_use) {
        context.lock.unlock();
        continue;
      }

      uint8_t* ptr = memory()->TranslatePhysical(context.guest_ptr);
      auto data = XMAContextData(ptr);

      if (data.input_buffer_0_valid || data.input_buffer_1_valid) {
        // A buffer is valid. Run the decoder!

        // Reset valid flags
        data.input_buffer_0_valid = 0;
        data.input_buffer_1_valid = 0;
        data.output_buffer_valid = 0;

        // Translate pointers for future use.
        auto in0 = memory()->TranslatePhysical(data.input_buffer_0_ptr);
        auto in1 = memory()->TranslatePhysical(data.input_buffer_1_ptr);
        auto out = memory()->TranslatePhysical(data.output_buffer_ptr);

        // I haven't seen this be used yet.
        assert(!data.input_buffer_1_block_count);

        // What I see:
        // XMA outputs 2 bytes per sample
        // 512 samples per frame (128 per subframe)
        // Max output size is data.output_buffer_block_count * 256

        // This decoder is fed packets (max 4095 per buffer)
        // Packets contain "some" frames
        // 32bit header (big endian)

        // Frames are the smallest thing the SPUs can decode.
        // They usually can span packets (libav handles this)

        // Sample rates (data.sample_rate):
        // 0 - 24 kHz ?
        // 1 - 32 kHz
        // 2 - 44.1 kHz ?
        // 3 - 48 kHz ?

        // SPUs also support stereo decoding. (data.is_stereo)

        while (true) {
          // Initial check - see if we've finished with the input
          // TODO - Probably need to move this, I think it might skip the very
          // last packet (see the call to PreparePacket)
          size_t input_size = (data.input_buffer_0_block_count +
                              data.input_buffer_1_block_count) * 2048;
          size_t input_offset = (data.input_buffer_read_offset / 8 - 4);
          size_t input_remaining = input_size - input_offset;
          if (input_remaining == 0) {
            // We're finished!
            break;
          }

          // Now check the output buffer.
          size_t output_size = data.output_buffer_block_count * 256;
          size_t output_offset = data.output_buffer_write_offset * 256;
          size_t output_remaining = output_size - output_offset;
          if (output_remaining == 0) {
            // Can't write any more data. Break.
            // The game will kick us again with a new output buffer later.
            break;
          }

          // This'll copy audio samples into the output buffer.
          // The samples need to be 2 bytes long!
          // Copies one frame at a time, so keep calling this until size == 0
          int read = context.decoder->DecodePacket(out, output_offset,
                                                   output_remaining);
          if (read < 0) {
            XELOGAPU("APU failed to decode packet (returned %.8X)", -read);
            context.decoder->DiscardPacket();

            // TODO: Set error state

            break;
          }

          if (read == 0) {
            // Select sample rate.
            int sample_rate = 0;
            if (data.sample_rate == 0) {
              // TODO: Test this
              sample_rate = 24000;
            } else if (data.sample_rate == 1) {
              sample_rate = 32000;
            } else if (data.sample_rate == 2) {
              // TODO: Test this
              sample_rate = 44100;
            } else if (data.sample_rate == 3) {
              // TODO: Test this
              sample_rate = 48000;
            }

            // Channels
            int channels = 1;
            if (data.is_stereo == 1) {
              channels = 2;
            }

            // New packet time.
            // TODO: Select input buffer 1 if necessary.
            auto packet = in0 + input_offset;
            context.decoder->PreparePacket(packet, 2048, sample_rate, channels);
            input_offset += 2048;
          }

          output_offset += read;

          // blah copy these back to the context
          data.input_buffer_read_offset = (input_offset + 4) * 8;
          data.output_buffer_write_offset = output_offset / 256;
        }

        data.Store(ptr);
      }

      context.lock.unlock();
    }
  }
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  worker_running_ = false;
  SetEvent(client_wait_handles_[maximum_client_count_]);
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();

  decoder_running_ = false;
  decoder_fence_.Signal();
  worker_thread_.reset();

  memory()->SystemHeapFree(registers_.xma_context_array_ptr);
}

uint32_t AudioSystem::AllocateXmaContext() {
  std::lock_guard<std::mutex> lock(lock_);

  for (uint32_t n = 0; n < kXmaContextCount; n++) {
    XMAContext& context = xma_context_array_[n];
    if (!context.in_use) {
      context.in_use = true;
      return context.guest_ptr;
    }
  }

  return 0;
}

void AudioSystem::ReleaseXmaContext(uint32_t guest_ptr) {
  std::lock_guard<std::mutex> lock(lock_);

  // Find it in the list.
  for (uint32_t n = 0; n < kXmaContextCount; n++) {
    XMAContext& context = xma_context_array_[n];
    if (context.guest_ptr == guest_ptr) {
      // Found it!
      // Lock it in case the decoder thread is working on it now
      context.lock.lock();

      context.in_use = false;
      auto context_ptr = memory()->TranslateVirtual(guest_ptr);
      std::memset(context_ptr, 0, kXmaContextSize); // Zero it.
      context.decoder->DiscardPacket();
      
      context.lock.unlock();
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  assert_true(unused_clients_.size());
  std::lock_guard<std::mutex> lock(lock_);

  auto index = unused_clients_.front();

  auto wait_handle = client_wait_handles_[index];
  ResetEvent(wait_handle);
  AudioDriver* driver;
  auto result = CreateDriver(index, wait_handle, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  unused_clients_.pop();

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  std::lock_guard<std::mutex> lock(lock_);
  assert_true(index < maximum_client_count_);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
  ResetEvent(client_wait_handles_[index]);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  std::lock_guard<std::mutex> lock(lock_);
  assert_true(index < maximum_client_count_);
  DestroyDriver(clients_[index].driver);
  clients_[index] = {0};
  unused_clients_.push(index);
  ResetEvent(client_wait_handles_[index]);
}

// free60 may be useful here, however it looks like it's using a different
// piece of hardware:
// https://github.com/Free60Project/libxenon/blob/master/libxenon/drivers/xenon_sound/sound.c

uint64_t AudioSystem::ReadRegister(uint32_t addr) {
  uint32_t r = addr & 0xFFFF;
  XELOGAPU("ReadRegister(%.4X)", r);
  // 1800h is read on startup and stored -- context? buffers?
  // 1818h is read during a lock?

  assert_true(r % 4 == 0);
  uint32_t value = register_file_[r / 4];

  // 1818 is rotating context processing # set to hardware ID of context being
  // processed.
  // If bit 200h is set, the locking code will possibly collide on hardware IDs
  // and error out, so we should never set it (I think?).
  if (r == 0x1818) {
    // To prevent games from seeing a stuck XMA context, return a rotating
    // number
    registers_.current_context = registers_.next_context;
    registers_.next_context = (registers_.next_context + 1) % kXmaContextCount;
    value = registers_.current_context;
  }

  value = xe::byte_swap(value);
  return value;
}

void AudioSystem::WriteRegister(uint32_t addr, uint64_t value) {
  uint32_t r = addr & 0xFFFF;
  value = xe::byte_swap(uint32_t(value));
  XELOGAPU("WriteRegister(%.4X, %.8X)", r, value);
  // 1804h is written to with 0x02000000 and 0x03000000 around a lock operation

  assert_true(r % 4 == 0);
  register_file_[r / 4] = uint32_t(value);

  if (r >= 0x1940 && r <= 0x1940 + 9 * 4) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // Basically, this kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array
    for (int i = 0; value && i < 32; ++i) {
      if (value & 1) {
        uint32_t context_id = i + (r - 0x1940) / 4 * 32;
        XMAContext& context = xma_context_array_[context_id];

        context.lock.lock();
        auto context_ptr = memory()->TranslateVirtual(context.guest_ptr);
        XMAContextData data(context_ptr);

        XELOGAPU("AudioSystem: kicking context %d (%d/%d bytes)", context_id,
                 data.input_buffer_read_offset, data.input_buffer_0_block_count
                 * XMAContextData::kBytesPerBlock);

        // Reset valid flags so our audio decoder knows to process this one
        data.input_buffer_0_valid = data.input_buffer_0_ptr != 0;
        data.input_buffer_1_valid = data.input_buffer_1_ptr != 0;
        data.output_buffer_write_offset = 0;

        data.Store(context_ptr);
        context.lock.unlock();

        // Signal the decoder thread
        decoder_fence_.Signal();
      }
      value >>= 1;
    }
  } else if (r >= 0x1A40 && r <= 0x1A40 + 9 * 4) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    for (int i = 0; value && i < 32; ++i) {
      if (value & 1) {
        uint32_t context_id = i + (r - 0x1A40) / 4 * 32;
        XELOGAPU("AudioSystem: set context lock %d", context_id);

        // TODO: Find the correct way to lock/unlock this.
        // I thought we could lock it here, unlock it in the kick but that
        // doesn't seem to work
        XMAContext& context = xma_context_array_[context_id];
      }
      value >>= 1;
    }
  } else if (r >= 0x1A80 && r <= 0x1A80 + 9 * 4) {
    // Context clear command.
    // This will reset the given hardware contexts.
    for (int i = 0; value && i < 32; ++i) {
      if (value & 1) {
        uint32_t context_id = i + (r - 0x1A80) / 4 * 32;
        XELOGAPU("AudioSystem: reset context %d", context_id);

        // TODO(benvanik): something?
        uint32_t guest_ptr =
            registers_.xma_context_array_ptr + context_id * kXmaContextSize;
        auto context_ptr = memory()->TranslateVirtual(guest_ptr);
      }
      value >>= 1;
    }
  } else {
    value = value;
  }
}

}  // namespace apu
}  // namespace xe
