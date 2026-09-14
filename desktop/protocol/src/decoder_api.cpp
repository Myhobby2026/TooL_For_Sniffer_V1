// -----------------------------------------------------------------------------
// decoder_api.cpp -- see decoder_api.h.
// -----------------------------------------------------------------------------
#include "usn/protocol/decoder_api.h"

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace usn::protocol {

std::string_view nameOf(ConfigType const type) noexcept {
    switch (type) {
    case ConfigType::Bool:        return "bool";
    case ConfigType::Int:         return "int";
    case ConfigType::UInt:        return "uint";
    case ConfigType::Enum:        return "enum";
    case ConfigType::Double:      return "double";
    case ConfigType::Bytes:       return "bytes";
    case ConfigType::String:      return "string";
    case ConfigType::ChannelRole: return "channel-role";
    }
    return "unknown";
}

const ConfigValue* DecoderConfiguration::find(std::string_view const key) const noexcept {
    auto const it = std::find_if(parameters.begin(), parameters.end(),
                                 [key](const ConfigValue& v) { return v.key == key; });
    return it == parameters.end() ? nullptr : &(*it);
}

const ChannelBinding* DecoderConfiguration::bindingFor(ChannelRole const role) const noexcept {
    auto const it = std::find_if(bindings.begin(), bindings.end(),
                                 [role](const ChannelBinding& b) { return b.role == role; });
    return it == bindings.end() ? nullptr : &(*it);
}

namespace {

bool typeMatches(ConfigType const expected, const FieldValue& value) {
    switch (expected) {
    case ConfigType::Bool:
        return std::holds_alternative<bool>(value);
    case ConfigType::Int:
        return std::holds_alternative<std::int64_t>(value);
    case ConfigType::UInt:
    case ConfigType::Enum:
        return std::holds_alternative<std::uint64_t>(value) ||
               std::holds_alternative<std::int64_t>(value);
    case ConfigType::Double:
        return std::holds_alternative<double>(value);
    case ConfigType::String:
        return std::holds_alternative<std::string>(value);
    case ConfigType::Bytes:
        return std::holds_alternative<std::vector<std::byte>>(value);
    case ConfigType::ChannelRole:
        return std::holds_alternative<std::uint64_t>(value);
    }
    return false;
}

std::int64_t asInt(const FieldValue& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
    }
    if (const auto* u = std::get_if<std::uint64_t>(&value)) {
        return static_cast<std::int64_t>(*u);
    }
    return 0;
}

}  // namespace

Status validateConfiguration(const DecoderInfo& info, const DecoderConfiguration& config) {
    if (config.decoderId != 0 && config.decoderId != info.id) {
        return Status::error(ErrorCode::DecoderConfigurationInvalid,
                             fmt::format("configuration targets decoder {} but was given to '{}'",
                                         config.decoderId, info.name));
    }
    for (const auto& param : info.parameters) {
        const auto* value = config.find(param.key);
        if (value == nullptr) {
            if (param.required) {
                return Status::error(ErrorCode::DecoderConfigurationInvalid,
                                     fmt::format("'{}': required parameter '{}' is missing",
                                                 info.name, param.key))
                    .withContext("decoder", info.name);
            }
            continue;
        }
        if (!typeMatches(param.type, value->value)) {
            return Status::error(
                       ErrorCode::DecoderConfigurationInvalid,
                       fmt::format("'{}': parameter '{}' expects {} but got '{}'", info.name,
                                   param.key, nameOf(param.type), toDisplayString(value->value)))
                .withContext("decoder", info.name);
        }
        if (param.range.has_value()) {
            auto const v = asInt(value->value);
            if (v < param.range->min || v > param.range->max) {
                return Status::error(
                           ErrorCode::DecoderConfigurationInvalid,
                           fmt::format("'{}': parameter '{}' = {} is outside [{}, {}]", info.name,
                                       param.key, v, param.range->min, param.range->max))
                    .withContext("decoder", info.name);
            }
        }
        if (param.type == ConfigType::Enum && !param.options.empty()) {
            auto const v = asInt(value->value);
            bool found = false;
            for (const auto& option : param.options) {
                if (option.value == v) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::string allowed;
                for (const auto& option : param.options) {
                    if (!allowed.empty()) {
                        allowed += ", ";
                    }
                    allowed += fmt::format("{} ({})", option.label, option.value);
                }
                return Status::error(ErrorCode::DecoderConfigurationInvalid,
                                     fmt::format("'{}': parameter '{}' = {} is not one of: {}",
                                                 info.name, param.key, v, allowed))
                    .withContext("decoder", info.name);
            }
        }
    }

    // Unknown parameters are rejected rather than ignored: a typo in a saved
    // workspace would otherwise silently decode with defaults.
    for (const auto& value : config.parameters) {
        bool known = false;
        for (const auto& param : info.parameters) {
            if (param.key == value.key) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Status::error(ErrorCode::DecoderConfigurationInvalid,
                                 fmt::format("'{}': unknown parameter '{}'", info.name, value.key))
                .withContext("decoder", info.name);
        }
    }

    for (auto const role : info.requiredRoles) {
        if (config.bindingFor(role) == nullptr) {
            return Status::error(ErrorCode::DecoderConfigurationInvalid,
                                 fmt::format("'{}': required channel role '{}' is not bound",
                                             info.name, nameOf(role)))
                .withContext("decoder", info.name);
        }
    }
    for (const auto& binding : config.bindings) {
        if (!binding.channel.isValid()) {
            return Status::error(
                ErrorCode::DecoderConfigurationInvalid,
                fmt::format("'{}': role '{}' is bound to an invalid channel", info.name,
                            nameOf(binding.role)));
        }
    }
    return Status::success();
}

}  // namespace usn::protocol
