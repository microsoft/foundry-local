// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace fl {

namespace TelemetryInternal {

struct HostEnvironmentEvidence {
  bool docker_marker = false;
  bool podman_marker = false;
  bool kubernetes = false;
  bool aws_ecs = false;
  bool generic_container = false;
  bool android_emulator = false;
  bool apple_virtual_machine = false;
  std::string systemd_container;
  std::string cgroup;
  std::string dmi;
  std::string cpu_info;
  std::string kernel_release;
};

struct HostEnvironmentInfo {
  bool is_container;
  bool is_virtual_machine;
  bool is_emulator;
  const char* container_type;
  const char* virtualization_type;
  const char* environment_class;
  const char* detection_confidence;
  const char* device_id_scope;
};

inline std::string_view TrimAscii(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

inline std::string ToLowerAscii(std::string_view value) {
  std::string out(value);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

inline bool ContainsAscii(std::string_view haystack, std::string_view needle) {
  return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

// Classifies only positive evidence. "undetected" deliberately does not claim bare metal.
inline HostEnvironmentInfo ClassifyHostEnvironment(const HostEnvironmentEvidence& evidence) {
  const std::string container_name = ToLowerAscii(TrimAscii(evidence.systemd_container));
  const std::string combined_container_evidence = ToLowerAscii(evidence.cgroup + " " + container_name);

  const char* container_type = "none";
  int container_confidence = 0;
  if (evidence.kubernetes || ContainsAscii(combined_container_evidence, "kubepods")) {
    container_type = "kubernetes";
    container_confidence = 2;
  } else if (evidence.aws_ecs) {
    container_type = "amazonECS";
    container_confidence = 2;
  } else if (evidence.podman_marker || ContainsAscii(combined_container_evidence, "libpod") ||
             ContainsAscii(combined_container_evidence, "podman")) {
    container_type = "podman";
    container_confidence = 2;
  } else if (evidence.docker_marker || ContainsAscii(combined_container_evidence, "docker")) {
    container_type = "docker";
    container_confidence = 2;
  } else if (ContainsAscii(combined_container_evidence, "containerd")) {
    container_type = "containerd";
    container_confidence = 1;
  } else if (ContainsAscii(combined_container_evidence, "lxc")) {
    container_type = "lxc";
    container_confidence = 1;
  } else if (!container_name.empty() && container_name != "none") {
    container_type = "other";
    container_confidence = 2;
  } else if (evidence.generic_container) {
    container_type = "other";
    container_confidence = 1;
  }

  const std::string dmi = ToLowerAscii(evidence.dmi);
  const std::string cpu_info = ToLowerAscii(evidence.cpu_info);
  const std::string kernel_release = ToLowerAscii(evidence.kernel_release);
  const char* virtualization_type = "none";
  int virtualization_confidence = 0;
  if (evidence.android_emulator) {
    virtualization_type = "androidEmulator";
    virtualization_confidence = 2;
  } else if (evidence.apple_virtual_machine) {
    virtualization_type = "appleVirtualMachine";
    virtualization_confidence = 2;
  } else if (kernel_release.find("microsoft") != std::string::npos ||
             kernel_release.find("wsl") != std::string::npos) {
    virtualization_type = "wsl";
    virtualization_confidence = 2;
  } else if (dmi.find("vmware") != std::string::npos) {
    virtualization_type = "vmware";
    virtualization_confidence = 2;
  } else if (dmi.find("virtualbox") != std::string::npos || dmi.find("innotek") != std::string::npos) {
    virtualization_type = "virtualBox";
    virtualization_confidence = 2;
  } else if (dmi.find("microsoft corporation") != std::string::npos &&
             dmi.find("virtual machine") != std::string::npos) {
    virtualization_type = "hyperV";
    virtualization_confidence = 2;
  } else if (dmi.find("amazon ec2") != std::string::npos) {
    virtualization_type = "amazonEC2";
    virtualization_confidence = 2;
  } else if (dmi.find("google compute engine") != std::string::npos) {
    virtualization_type = "googleComputeEngine";
    virtualization_confidence = 2;
  } else if (dmi.find("openstack") != std::string::npos) {
    virtualization_type = "openStack";
    virtualization_confidence = 2;
  } else if (dmi.find("kvm") != std::string::npos) {
    virtualization_type = "kvm";
    virtualization_confidence = 2;
  } else if (dmi.find("qemu") != std::string::npos) {
    virtualization_type = "qemu";
    virtualization_confidence = 2;
  } else if (dmi.find("xen") != std::string::npos) {
    virtualization_type = "xen";
    virtualization_confidence = 2;
  } else if (dmi.find("parallels") != std::string::npos) {
    virtualization_type = "parallels";
    virtualization_confidence = 2;
  } else if (dmi.find("bhyve") != std::string::npos) {
    virtualization_type = "bhyve";
    virtualization_confidence = 2;
  } else if (cpu_info.find("hypervisor") != std::string::npos) {
    virtualization_type = "other";
    virtualization_confidence = 1;
  }

  const bool is_container = container_confidence != 0;
  const bool is_virtual_machine = virtualization_confidence != 0;
  const bool is_emulator = evidence.android_emulator;
  const char* environment_class = "undetected";
  if (is_container && is_virtual_machine) {
    environment_class = "containerOnVirtualMachine";
  } else if (is_container) {
    environment_class = "container";
  } else if (is_emulator) {
    environment_class = "emulator";
  } else if (is_virtual_machine) {
    environment_class = "virtualMachine";
  }

  const int confidence =
      container_confidence > virtualization_confidence ? container_confidence : virtualization_confidence;
  return {is_container,
          is_virtual_machine,
          is_emulator,
          container_type,
          virtualization_type,
          environment_class,
          confidence == 2 ? "high" : confidence == 1 ? "medium"
                                                     : "none",
          is_container ? "container" : is_virtual_machine ? "virtualMachine"
                                                          : "installation"};
}

}  // namespace TelemetryInternal

/// Static helpers for telemetry runtime gating.
class TelemetryEnvironment {
 public:
  /// Returns true if any well-known CI environment variable is set to a truthy
  /// value. The set matches neutron-server's TelemetryEnvironment.cs.
  /// In CI, OneDsTelemetry skips Initialize entirely — no 1DS events emitted.
  static bool IsCiEnvironment();

  /// Returns true when ORT_TELEMETRY_DISABLED requests suppression of non-essential telemetry.
  static bool IsTelemetryDisabledByEnvVar();

  /// Truthy-value semantics: a non-empty, non-whitespace string whose trimmed
  /// value is not "0", "false", "no", or "off" (case-insensitive).
  static bool IsTruthyValue(std::string_view value);

  /// Read an env var (cross-platform). Returns empty string if unset.
  static std::string GetEnv(const char* name);
};

}  // namespace fl
