/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/debug/gdb/gdbstub.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "xenia/base/clock.h"
#include "xenia/base/debugging.h"
#include "xenia/base/fuzzy.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace debug {
namespace gdb {

using xe::cpu::Breakpoint;
using xe::kernel::XModule;
using xe::kernel::XObject;
using xe::kernel::XThread;

enum class GdbStubControl : char {
  Ack = '+',
  Nack = '-',
  PacketStart = '$',
  PacketEnd = '#',
  Interrupt = '\03',
};

constexpr const char* kGdbReplyOK = "OK";
constexpr const char* kGdbReplyError = "E01";

constexpr int kSignalSigtrap = 5;

// must start with l for debugger to accept it
// TODO: add power-altivec.xml (and update ReadRegister to support it)
constexpr char target_xml[] =
    R"(l<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
<feature name="org.gnu.gdb.power.core">
  <reg name="r0" bitsize="32" type="uint32"/>
  <reg name="r1" bitsize="32" type="uint32"/>
  <reg name="r2" bitsize="32" type="uint32"/>
  <reg name="r3" bitsize="32" type="uint32"/>
  <reg name="r4" bitsize="32" type="uint32"/>
  <reg name="r5" bitsize="32" type="uint32"/>
  <reg name="r6" bitsize="32" type="uint32"/>
  <reg name="r7" bitsize="32" type="uint32"/>
  <reg name="r8" bitsize="32" type="uint32"/>
  <reg name="r9" bitsize="32" type="uint32"/>
  <reg name="r10" bitsize="32" type="uint32"/>
  <reg name="r11" bitsize="32" type="uint32"/>
  <reg name="r12" bitsize="32" type="uint32"/>
  <reg name="r13" bitsize="32" type="uint32"/>
  <reg name="r14" bitsize="32" type="uint32"/>
  <reg name="r15" bitsize="32" type="uint32"/>
  <reg name="r16" bitsize="32" type="uint32"/>
  <reg name="r17" bitsize="32" type="uint32"/>
  <reg name="r18" bitsize="32" type="uint32"/>
  <reg name="r19" bitsize="32" type="uint32"/>
  <reg name="r20" bitsize="32" type="uint32"/>
  <reg name="r21" bitsize="32" type="uint32"/>
  <reg name="r22" bitsize="32" type="uint32"/>
  <reg name="r23" bitsize="32" type="uint32"/>
  <reg name="r24" bitsize="32" type="uint32"/>
  <reg name="r25" bitsize="32" type="uint32"/>
  <reg name="r26" bitsize="32" type="uint32"/>
  <reg name="r27" bitsize="32" type="uint32"/>
  <reg name="r28" bitsize="32" type="uint32"/>
  <reg name="r29" bitsize="32" type="uint32"/>
  <reg name="r30" bitsize="32" type="uint32"/>
  <reg name="r31" bitsize="32" type="uint32"/>

  <reg name="pc" bitsize="32" type="code_ptr" regnum="64"/>
  <reg name="msr" bitsize="32" type="uint32"/>
  <reg name="cr" bitsize="32" type="uint32"/>
  <reg name="lr" bitsize="32" type="code_ptr"/>
  <reg name="ctr" bitsize="32" type="uint32"/>
  <reg name="xer" bitsize="32" type="uint32"/>
</feature>
<feature name="org.gnu.gdb.power.fpu">
  <reg name="f0" bitsize="64" type="ieee_double" regnum="32"/>
  <reg name="f1" bitsize="64" type="ieee_double"/>
  <reg name="f2" bitsize="64" type="ieee_double"/>
  <reg name="f3" bitsize="64" type="ieee_double"/>
  <reg name="f4" bitsize="64" type="ieee_double"/>
  <reg name="f5" bitsize="64" type="ieee_double"/>
  <reg name="f6" bitsize="64" type="ieee_double"/>
  <reg name="f7" bitsize="64" type="ieee_double"/>
  <reg name="f8" bitsize="64" type="ieee_double"/>
  <reg name="f9" bitsize="64" type="ieee_double"/>
  <reg name="f10" bitsize="64" type="ieee_double"/>
  <reg name="f11" bitsize="64" type="ieee_double"/>
  <reg name="f12" bitsize="64" type="ieee_double"/>
  <reg name="f13" bitsize="64" type="ieee_double"/>
  <reg name="f14" bitsize="64" type="ieee_double"/>
  <reg name="f15" bitsize="64" type="ieee_double"/>
  <reg name="f16" bitsize="64" type="ieee_double"/>
  <reg name="f17" bitsize="64" type="ieee_double"/>
  <reg name="f18" bitsize="64" type="ieee_double"/>
  <reg name="f19" bitsize="64" type="ieee_double"/>
  <reg name="f20" bitsize="64" type="ieee_double"/>
  <reg name="f21" bitsize="64" type="ieee_double"/>
  <reg name="f22" bitsize="64" type="ieee_double"/>
  <reg name="f23" bitsize="64" type="ieee_double"/>
  <reg name="f24" bitsize="64" type="ieee_double"/>
  <reg name="f25" bitsize="64" type="ieee_double"/>
  <reg name="f26" bitsize="64" type="ieee_double"/>
  <reg name="f27" bitsize="64" type="ieee_double"/>
  <reg name="f28" bitsize="64" type="ieee_double"/>
  <reg name="f29" bitsize="64" type="ieee_double"/>
  <reg name="f30" bitsize="64" type="ieee_double"/>
  <reg name="f31" bitsize="64" type="ieee_double"/>

  <reg name="fpscr" bitsize="32" group="float" regnum="70"/>
</feature>
</target>
)";

std::string u64_to_padded_hex(uint64_t value) {
  return fmt::format("{:016x}", value);
}

std::string u32_to_padded_hex(uint32_t value) {
  return fmt::format("{:08x}", value);
}

template <typename T>
T hex_to(std::string_view val) {
  T result;
  std::from_chars(val.data(), val.data() + val.size(), result, 16);

  return result;
}

constexpr auto& hex_to_u8 = hex_to<uint8_t>;
constexpr auto& hex_to_u32 = hex_to<uint32_t>;
constexpr auto& hex_to_u64 = hex_to<uint64_t>;

std::string to_hexbyte(uint8_t i) {
  std::string result = "00";
  uint8_t i1 = i & 0xF;
  uint8_t i2 = i >> 4;
  result[0] = i2 > 9 ? 'a' + i2 - 10 : '0' + i2;
  result[1] = i1 > 9 ? 'a' + i1 - 10 : '0' + i1;
  return result;
}

// Convert a hex char (0-9, a-f, A-F) to a byte
uint8_t from_hexchar(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return 0;
}

std::string GDBStub::DebuggerDetached() {
  // Delete all breakpoints
  auto& state = cache_.breakpoints;

  for (auto& breakpoint : state.all_breakpoints) {
    processor_->RemoveBreakpoint(breakpoint.get());
  }

  state.code_breakpoints_by_guest_address.clear();
  state.all_breakpoints.clear();

  if (processor_->execution_state() == cpu::ExecutionState::kPaused) {
    ExecutionContinue();
  }

  return kGdbReplyOK;
}

std::string GDBStub::ReadRegister(xe::cpu::ThreadDebugInfo* thread,
                                  uint32_t rid) {
  // Send registers as 32-bit, otherwise some debuggers will switch to 64-bit
  // mode (eg. IDA will switch to 64-bit and refuse to allow decompiler to work
  // with it)
  //
  // TODO: add altivec/VMX registers here...
  //
  // ids from gdb/features/rs6000/powerpc-64.c
  switch (rid) {
    // pc
    case 64: {
      // If we recently hit a BP then debugger is likely asking for registers
      // for it
      //
      // Lie about the PC and say it's the BP address, since PC might not always
      // match
      if (cache_.notify_bp_guest_address != -1) {
        auto ret = u32_to_padded_hex((uint32_t)cache_.notify_bp_guest_address);
        cache_.notify_bp_guest_address = -1;
        return ret;
      }
      // Search for first frame that has guest_pc attached, GDB doesn't care
      // about host
      for (auto& frame : thread->frames) {
        if (frame.guest_pc != 0) {
          return u32_to_padded_hex((uint32_t)frame.guest_pc);
        }
      }
      return u32_to_padded_hex(0);
    }
    // msr?
    case 65:
      return std::string(8, 'x');
    case 66:
      return u32_to_padded_hex((uint32_t)thread->guest_context.cr());
    case 67:
      return u32_to_padded_hex((uint32_t)thread->guest_context.lr);
    case 68:
      return u32_to_padded_hex((uint32_t)thread->guest_context.ctr);
    // xer
    case 69:
      return std::string(8, 'x');
    // fpscr
    case 70:
      return std::string(8, 'x');
    default:
      if (rid > 70) return "";
      return (rid > 31) ? u64_to_padded_hex(*(uint64_t*)&(
                              thread->guest_context.f[rid - 32]))  // fpr
                        : u32_to_padded_hex(
                              (uint32_t)thread->guest_context.r[rid]);  // gpr
  }
}

GDBStub::GDBStub(Emulator* emulator, int listen_port)
    : emulator_(emulator),
      processor_(emulator->processor()),
      listen_port_(listen_port) {}

GDBStub::~GDBStub() { stop_thread_ = true; }

std::unique_ptr<GDBStub> GDBStub::Create(Emulator* emulator, int listen_port) {
  std::unique_ptr<GDBStub> debugger(new GDBStub(emulator, listen_port));
  if (!debugger->Initialize()) {
    xe::FatalError("GDBStub::Create: Failed to initialize GDB stub");
    return nullptr;
  }
  return debugger;
}

bool GDBStub::Initialize() {
  socket_ = xe::SocketServer::Create(
      listen_port_, [this](std::unique_ptr<Socket> client) { Listen(client); });

  UpdateCache();
  return true;
}

void GDBStub::Listen(std::unique_ptr<Socket>& client) {
  // Client is connected - pause execution
  ExecutionPause();
  UpdateCache();

  client->set_nonblocking(true);

  std::string receive_buffer;

  while (!stop_thread_) {
    if (!client->is_connected()) {
      break;
    }

    if (!ProcessIncomingData(client, receive_buffer)) {
      if (!client->is_connected()) {
        break;
      }
      // No data available, can do other work or sleep
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Check if we need to notify client about anything...
    {
      std::unique_lock<std::mutex> lock(mtx_);
      if (cache_.notify_stopped) {
        if (cache_.notify_bp_thread_id != -1)
          cache_.cur_thread_id = cache_.notify_bp_thread_id;
        SendPacket(client, GetThreadStateReply(cache_.notify_bp_thread_id,
                                               kSignalSigtrap));
        cache_.notify_bp_thread_id = -1;
        cache_.notify_stopped = false;
      }
    }
  }
}

void GDBStub::SendPacket(std::unique_ptr<Socket>& client,
                         const std::string& data) {
  std::stringstream ss;
  ss << char(GdbStubControl::PacketStart) << data
     << char(GdbStubControl::PacketEnd);

  uint8_t checksum = 0;
  for (char c : data) checksum += c;

  ss << std::hex << std::setw(2) << std::setfill('0') << (checksum & 0xff);
  std::string packet = ss.str();

  client->Send(packet.c_str(), packet.size());
}

#ifdef DEBUG
std::string GetPacketFriendlyName(const std::string& packetCommand) {
  static const std::unordered_map<std::string, std::string> command_names = {
      {"?", "StartupQuery"},
      {"!", "EnableExtendedMode"},
      {"p", "ReadRegister"},
      {"P", "WriteRegister"},
      {"g", "ReadAllRegisters"},
      {"C", "Continue"},
      {"c", "continue"},
      {"s", "step"},
      {"vAttach", "vAttach"},
      {"m", "MemRead"},
      {"H", "SetThreadId"},
      {"Z", "CreateCodeBreakpoint"},
      {"z", "DeleteCodeBreakpoint"},
      {"qXfer", "Xfer"},
      {"qSupported", "Supported"},
      {"qfThreadInfo", "qfThreadInfo"},
      {"qC", "GetThreadId"},
      {"D", "Detach"},
      {"\03", "Break"},
  };

  std::string packet_name = "";
  auto it = command_names.find(packetCommand);
  if (it != command_names.end()) {
    packet_name = it->second;
  }

  return packet_name;
}
#endif

bool GDBStub::ProcessIncomingData(std::unique_ptr<Socket>& client,
                                  std::string& receive_buffer) {
  char buffer[1024];
  size_t received = client->Receive(buffer, sizeof(buffer));
  if (received == -1 || received == 0) {
    return false;
  }

  receive_buffer.append(buffer, received);

  // Hacky interrupt '\03' packet handling, some reason checksum isn't
  // attached to this?
  bool isInterrupt =
      buffer[0] == char(GdbStubControl::Interrupt) && received == 1;

  // Check if we've received a full packet yet, if not exit and allow caller
  // to try again
  size_t packet_end;
  while (isInterrupt ||
         (packet_end = receive_buffer.find('#')) != std::string::npos) {
    if (isInterrupt || packet_end + 2 < receive_buffer.length()) {
      std::string current_packet;
      if (isInterrupt) {
        current_packet = char(GdbStubControl::Interrupt);
        receive_buffer = "";
        isInterrupt = false;
      } else {
        current_packet = receive_buffer.substr(0, packet_end + 3);
        receive_buffer = receive_buffer.substr(packet_end + 3);
      }

      GDBCommand command;
      if (ParsePacket(current_packet, command)) {
#ifdef DEBUG
        auto packet_name = GetPacketFriendlyName(command.cmd);

        debugging::DebugPrint("GDBStub: Packet {}({})\n",
                              packet_name.empty() ? command.cmd : packet_name,
                              command.data);
#endif

        GdbStubControl result = GdbStubControl::Ack;
        client->Send(&result, 1);
        std::string response = HandleGDBCommand(command);
        SendPacket(client, response);
      } else {
        GdbStubControl result = GdbStubControl::Nack;
        client->Send(&result, 1);
      }
    } else {
      break;
    }
  }

  return true;
}

bool GDBStub::ParsePacket(const std::string& packet, GDBCommand& out_cmd) {
  // Index to track position in packet
  size_t buffer_index = 0;

  // Read a character from the buffer and increment index
  auto ReadCharFromBuffer = [&]() -> char {
    if (buffer_index >= packet.size()) {
      return '\0';
    }
    return packet[buffer_index++];
  };

  // Parse two hex digits from buffer
  auto ReadHexByteFromBuffer = [&]() -> char {
    if (buffer_index + 2 > packet.size()) {
      return 0;
    }
    char high = packet[buffer_index++];
    char low = packet[buffer_index++];
    return (from_hexchar(high) << 4) | from_hexchar(low);
  };

  // Read the first character from the buffer
  char c = ReadCharFromBuffer();

  // Expecting start of packet '$'
  if (c != char(GdbStubControl::PacketStart)) {
    // gdb starts conversation with + for some reason
    if (c == char(GdbStubControl::Ack)) {
      c = ReadCharFromBuffer();
    }
    // and IDA sometimes has double +, grr
    if (c == char(GdbStubControl::Ack)) {
      c = ReadCharFromBuffer();
    }
    // Interrupt is special, handle it without checking checksum
    if (c == char(GdbStubControl::Interrupt)) {
      out_cmd.cmd = char(GdbStubControl::Interrupt);
      out_cmd.data = "";
      out_cmd.checksum = 0;
      return true;
    }
    if (c != char(GdbStubControl::PacketStart)) {
      return false;
    }
  }

  // Clear packet data
  out_cmd.cmd = "";
  out_cmd.data = "";
  out_cmd.checksum = 0;
  bool cmd_part = true;
  uint8_t checksum = 0;

  // Parse packet content
  while (true) {
    c = ReadCharFromBuffer();

    // If we reach the end of the buffer or hit '#', stop
    if (c == '\0' || c == char(GdbStubControl::PacketEnd)) {
      break;
    }

    checksum = (checksum + static_cast<uint8_t>(c)) % 256;

    // Handle escaped characters
    if (c == '}') {
      c = ReadCharFromBuffer() ^ 0x20;  // Read next char and XOR with 0x20
      checksum = (checksum + static_cast<uint8_t>(c)) % 256;
    }

    // Command-data splitters: check for ':', '.', or ';'
    if (cmd_part && (c == ':' || c == '.' || c == ';')) {
      cmd_part = false;
    }

    if (cmd_part) {
      out_cmd.cmd += c;

      // Only 'q' and 'v' commands can have multi-char commands
      if (out_cmd.cmd.length() == 1 && c != 'q' && c != 'v') {
        cmd_part = false;
      }
    } else {
      out_cmd.data += c;
    }
  }

  // Now read & compare the checksum
  out_cmd.checksum = ReadHexByteFromBuffer();
  return out_cmd.checksum == checksum;
}

void GDBStub::UpdateCache() {
  auto kernel_state = emulator_->kernel_state();
  auto object_table = kernel_state->object_table();

  std::unique_lock<std::mutex> lock(mtx_);

  cache_.is_stopped =
      processor_->execution_state() != cpu::ExecutionState::kRunning;
  cache_.notify_stopped = cache_.is_stopped;
  if (!cache_.is_stopped) {
    // Early exit - the rest of the data is kept stale on purpose.
    return;
  }

  // Fetch module listing.
  // We hold refs so that none are unloaded.
  cache_.modules =
      object_table->GetObjectsByType<XModule>(XObject::Type::Module);

  cache_.thread_debug_infos = processor_->QueryThreadDebugInfos();
  cache_.cur_thread_id = cache_.thread_debug_infos[0]->thread_id;
}

std::string GDBStub::ReadRegister(const std::string& data) {
  uint32_t rid = hex_to_u32(data);
  std::string result = ReadRegister(cache_.cur_thread_info(), rid);
  if (result.empty()) {
    return kGdbReplyError;  // TODO: is this error correct?
  }
  return result;
}

std::string GDBStub::ReadRegisters() {
  std::string result;
  result.reserve(68 * 16 + 3 * 8);
  for (int i = 0; i < 71; ++i) {
    result += ReadRegister(cache_.cur_thread_info(), i);
  }
  return result;
}

std::string GDBStub::ExecutionPause() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionPause\n");
#endif
  processor_->Pause();
  return kGdbReplyOK;
}

std::string GDBStub::ExecutionContinue() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionContinue\n");
#endif
  processor_->Continue();
  return kGdbReplyOK;
}

std::string GDBStub::ExecutionStep() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionStep (thread {})\n",
                        cache_.last_bp_thread_id);
#endif

  if (cache_.last_bp_thread_id != -1)
    processor_->StepGuestInstruction(cache_.last_bp_thread_id);

  return kGdbReplyOK;
}

std::string GDBStub::ReadMemory(const std::string& data) {
  auto s = data.find(',');
  uint32_t addr = hex_to_u32(data.substr(0, s));
  uint32_t len = hex_to_u32(data.substr(s + 1));
  std::string result;
  result.reserve(len * 2);

  // TODO: is there a better way to check if addr is valid?
  auto* heap = processor_->memory()->LookupHeap(addr);
  if (!heap) {
    return kGdbReplyError;
  }
  uint32_t protect = 0;
  if (!heap->QueryProtect(addr, &protect) ||
      (protect & kMemoryProtectRead) != kMemoryProtectRead) {
    return kGdbReplyError;
  }

  auto* mem = processor_->memory()->TranslateVirtual(addr);
  for (uint32_t i = 0; i < len; ++i) {
    result += to_hexbyte(*mem);
    mem++;
  }

  if (len && result.empty()) {
    return kGdbReplyError;  // nothing read
  }

  return result;
}

std::string GDBStub::BuildTargetXml() { return target_xml; }

std::string GDBStub::BuildThreadList() {
  std::string buffer;
  buffer += "l<?xml version=\"1.0\"?>";
  buffer += "<threads>";

  for (int i = 0; i < cache_.thread_debug_infos.size(); i++) {
    auto& thread = cache_.thread_debug_infos[i];
    buffer += fmt::format(R"*(<thread id="{:x}" name="{}"></thread>)*",
                          thread->thread_id, thread->thread->thread_name());
  }

  buffer += "</threads>";
  return buffer;
}

std::string GDBStub::GetThreadStateReply(uint32_t thread_id, uint8_t signal) {
  constexpr int PC_REGISTER = 64;
  constexpr int LR_REGISTER = 67;

  auto* thread = cache_.thread_info(thread_id);

  if (thread_id != -1 && thread) {
    uint64_t pc_value = 0;
    for (auto& frame : thread->frames) {
      if (frame.guest_pc != 0) {
        pc_value = frame.guest_pc;
        break;
      }
    }

    // If BP was hit use the address of it, so debugger can match it up to its
    // BP list
    if (cache_.notify_bp_guest_address != -1) {
      pc_value = cache_.notify_bp_guest_address;
    }

    return fmt::format(
        "T{:02x}{:02x}:{};{:02x}:{};thread:{:x};", signal, PC_REGISTER,
        u32_to_padded_hex((uint32_t)pc_value), LR_REGISTER,
        u32_to_padded_hex((uint32_t)thread->guest_context.lr), thread_id);
  }
  return "S05";
}

bool GDBStub::CreateCodeBreakpoint(uint64_t address) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Adding breakpoint: {:X}\n", address);
#endif

  auto& state = cache_.breakpoints;
  auto breakpoint = std::make_unique<Breakpoint>(
      processor_, Breakpoint::AddressType::kGuest, address,
      [this](Breakpoint* breakpoint, cpu::ThreadDebugInfo* thread_info,
             uint64_t host_address) {
        OnBreakpointHit(breakpoint, thread_info);
      });

  // Fetch list of host addrs used by the new BP
  std::vector<uintptr_t> host_addresses;
  breakpoint->ForEachHostAddress([&host_addresses](uintptr_t host_address) {
    host_addresses.push_back(host_address);
  });

  auto& map = state.code_breakpoints_by_guest_address;
  for (auto& kvp : map) {
    if (kvp.first == breakpoint->guest_address()) {
      return false;  // Already exists!
    }
    for (auto& host_address : host_addresses) {
      if (kvp.second->ContainsHostAddress(host_address)) {
        return false;  // Host addr is in use by another BP already
      }
    }
  }

  map.emplace(breakpoint->guest_address(), breakpoint.get());

  processor_->AddBreakpoint(breakpoint.get());
  state.all_breakpoints.emplace_back(std::move(breakpoint));

  return true;
}

void GDBStub::DeleteCodeBreakpoint(uint64_t address) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Deleting breakpoint: {:X}\n", address);
#endif
  auto* breakpoint = LookupBreakpointAtAddress(address);
  if (!breakpoint) {
    return;
  }
  DeleteCodeBreakpoint(breakpoint);
}

void GDBStub::DeleteCodeBreakpoint(Breakpoint* breakpoint) {
  auto& state = cache_.breakpoints;
  for (size_t i = 0; i < state.all_breakpoints.size(); ++i) {
    if (state.all_breakpoints[i].get() != breakpoint) {
      continue;
    }
    processor_->RemoveBreakpoint(breakpoint);

    auto& map = state.code_breakpoints_by_guest_address;
    auto it = map.find(breakpoint->guest_address());
    if (it != map.end()) {
      map.erase(it);
    }

    state.all_breakpoints.erase(state.all_breakpoints.begin() + i);
    break;
  }
}

Breakpoint* GDBStub::LookupBreakpointAtAddress(uint64_t address) {
  auto& state = cache_.breakpoints;
  auto& map = state.code_breakpoints_by_guest_address;
  auto it = map.find(static_cast<uint32_t>(address));
  return it == map.end() ? nullptr : it->second;
}

void GDBStub::OnFocus() {}

void GDBStub::OnDetached() {
  UpdateCache();

  // Remove all breakpoints.
  while (!cache_.breakpoints.all_breakpoints.empty()) {
    DeleteCodeBreakpoint(cache_.breakpoints.all_breakpoints.front().get());
  }
}

void GDBStub::OnExecutionPaused() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionPaused\n");
#endif
  UpdateCache();
}

void GDBStub::OnExecutionContinued() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionContinued\n");
#endif
  UpdateCache();
}

void GDBStub::OnExecutionEnded() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionEnded\n");
#endif
  UpdateCache();
}

void GDBStub::OnStepCompleted(cpu::ThreadDebugInfo* thread_info) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnStepCompleted\n");
#endif
  // Some debuggers like IDA will remove the current breakpoint & step into next
  // instruction, only re-adding BP after it's told about the step
  cache_.notify_bp_thread_id = thread_info->thread_id;
  cache_.last_bp_thread_id = thread_info->thread_id;
  UpdateCache();
}

void GDBStub::OnBreakpointHit(Breakpoint* breakpoint,
                              cpu::ThreadDebugInfo* thread_info) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Breakpoint hit at {:X} (thread {})\n",
                        breakpoint->address(), thread_info->thread_id);
#endif

  cache_.notify_bp_guest_address = breakpoint->address();
  cache_.notify_bp_thread_id = thread_info->thread_id;
  cache_.last_bp_thread_id = thread_info->thread_id;
  UpdateCache();
}

std::string GDBStub::HandleGDBCommand(const GDBCommand& command) {
  static const std::unordered_map<std::string,
                                  std::function<std::string(const GDBCommand&)>>
      command_map = {
          // "sent when connection is first established to query the reason the
          // target halted"
          {"?",
           [&](const GDBCommand& cmd) {
             return "S05";  // tell debugger we're currently paused
           }},

          // Detach
          {"D", [&](const GDBCommand& cmd) { return DebuggerDetached(); }},

          // Enable extended mode
          {"!", [&](const GDBCommand& cmd) { return kGdbReplyOK; }},

          // Execution continue
          {"C", [&](const GDBCommand& cmd) { return ExecutionContinue(); }},
          // Execution continue
          {"c", [&](const GDBCommand& cmd) { return ExecutionContinue(); }},
          // Execution step
          {"s", [&](const GDBCommand& cmd) { return ExecutionStep(); }},
          // Execution interrupt
          {"\03", [&](const GDBCommand& cmd) { return ExecutionPause(); }},

          // Read memory
          {"m", [&](const GDBCommand& cmd) { return ReadMemory(cmd.data); }},
          // Read register
          {"p", [&](const GDBCommand& cmd) { return ReadRegister(cmd.data); }},
          // Write register
          {"P",
           [&](const GDBCommand& cmd) {
             return kGdbReplyOK;  // TODO: we'll just tell it write was fine
           }},
          // Read all registers
          {"g", [&](const GDBCommand& cmd) { return ReadRegisters(); }},

          // Attach to specific process ID - IDA used to send this, but doesn't
          // after some changes?
          {"vAttach", [&](const GDBCommand& cmd) { return "S05"; }},

          // Get current debugger thread ID
          {"qC",
           [&](const GDBCommand& cmd) {
             return "QC" + std::to_string(cache_.cur_thread_info()->thread_id);
           }},
          // Set current debugger thread ID
          {"H",
           [&](const GDBCommand& cmd) {
             // Reset to known good ID
             cache_.cur_thread_id =
                 cache_.thread_debug_infos.size()
                     ? cache_.thread_debug_infos[0]->thread_id
                     : -1;

             // Check if the desired thread ID exists
             int threadId = std::stol(cmd.data.substr(1), 0, 16);
             for (auto& thread : cache_.thread_debug_infos) {
               if (thread->thread_id == threadId) {
                 cache_.cur_thread_id = threadId;
                 break;
               }
             }

             return kGdbReplyOK;
           }},

          // Create breakpoint
          {"Z",
           [&](const GDBCommand& cmd) {
             auto& hex_addr = cmd.data.substr(2);
             uint64_t addr = std::stoull(hex_addr.substr(0, hex_addr.find(',')),
                                         nullptr, 16);

             return CreateCodeBreakpoint(addr) ? kGdbReplyOK : kGdbReplyError;
           }},
          // Delete breakpoint
          {"z",
           [&](const GDBCommand& cmd) {
             auto& hex_addr = cmd.data.substr(2);
             uint64_t addr = std::stoull(hex_addr.substr(0, hex_addr.find(',')),
                                         nullptr, 16);
             DeleteCodeBreakpoint(addr);
             return kGdbReplyOK;
           }},

          // Data transfer
          {"qXfer",
           [&](const GDBCommand& cmd) {
             auto param = cmd.data;
             if (param.length() > 0 && param[0] == ':') {
               param = param.substr(1);
             }
             auto sub_cmd = param.substr(0, param.find(':'));
             if (sub_cmd == "features") {
               return BuildTargetXml();
             } else if (sub_cmd == "threads") {
               return BuildThreadList();
             }
             return std::string(kGdbReplyError);
           }},
          // Supported features (TODO: memory map)
          {"qSupported",
           [&](const GDBCommand& cmd) {
             return "PacketSize=1024;qXfer:features:read+;qXfer:threads:read+";
           }},
          // Thread list (IDA requests this but ignores in favor of qXfer?)
          {"qfThreadInfo",
           [&](const GDBCommand& cmd) {
             std::string result;
             for (auto& thread : cache_.thread_debug_infos) {
               if (!result.empty()) result += ",";
               result += std::to_string(thread->thread_id);
             }
             return "m" + result;
           }},
      };

  auto it = command_map.find(command.cmd);
  if (it != command_map.end()) {
    return it->second(command);
  }

  return "";
}

}  // namespace gdb
}  // namespace debug
}  // namespace xe
