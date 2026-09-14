// -----------------------------------------------------------------------------
// trigger_node.cpp -- see trigger_node.h.
// -----------------------------------------------------------------------------
#include "usn/trigger/trigger_node.h"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace usn::trigger {

std::string_view nameOf(NodeKind const kind) noexcept {
    switch (kind) {
    case NodeKind::Edge:          return "edge";
    case NodeKind::Level:         return "level";
    case NodeKind::Pattern:       return "pattern";
    case NodeKind::PulseWidth:    return "pulse-width";
    case NodeKind::Timeout:       return "timeout";
    case NodeKind::ByteSequence:  return "byte-sequence";
    case NodeKind::ProtocolField: return "protocol-field";
    case NodeKind::Count:         return "count";
    case NodeKind::SearchExpr:    return "search-expr";
    case NodeKind::And:           return "and";
    case NodeKind::Or:            return "or";
    case NodeKind::Not:           return "not";
    case NodeKind::Sequence:      return "sequence";
    case NodeKind::Always:        return "always";
    case NodeKind::Never:         return "never";
    }
    return "unknown";
}

StatusOr<NodeKind> nodeKindFromName(std::string_view const name) {
    for (auto const k : {NodeKind::Edge, NodeKind::Level, NodeKind::Pattern, NodeKind::PulseWidth,
                         NodeKind::Timeout, NodeKind::ByteSequence, NodeKind::ProtocolField,
                         NodeKind::Count, NodeKind::SearchExpr, NodeKind::And, NodeKind::Or,
                         NodeKind::Not, NodeKind::Sequence, NodeKind::Always, NodeKind::Never}) {
        if (nameOf(k) == name) {
            return k;
        }
    }
    return Status::error(ErrorCode::ConfigurationError,
                         fmt::format("unknown trigger node kind '{}'", name));
}

std::string_view nameOf(ComparisonOp const op) noexcept {
    switch (op) {
    case ComparisonOp::Eq:       return "==";
    case ComparisonOp::Ne:       return "!=";
    case ComparisonOp::Lt:       return "<";
    case ComparisonOp::Le:       return "<=";
    case ComparisonOp::Gt:       return ">";
    case ComparisonOp::Ge:       return ">=";
    case ComparisonOp::In:       return "in";
    case ComparisonOp::Contains: return "contains";
    case ComparisonOp::Matches:  return "matches";
    }
    return "?";
}

std::string_view nameOf(Executability const exec) noexcept {
    switch (exec) {
    case Executability::DeviceAndHost: return "device+host";
    case Executability::HostOnly:      return "host-only";
    case Executability::Unsupported:   return "unsupported";
    }
    return "unknown";
}

// --- construction helpers ----------------------------------------------------

TriggerNode TriggerNode::always() {
    TriggerNode n;
    n.kind = NodeKind::Always;
    return n;
}

namespace {
TriggerNode makeEdge(ChannelId const ch, EdgeDirection const dir) {
    TriggerNode n;
    n.kind = NodeKind::Edge;
    n.channel = ch;
    n.edge = dir;
    return n;
}
}  // namespace

TriggerNode TriggerNode::rising(ChannelId const ch) { return makeEdge(ch, EdgeDirection::Rising); }
TriggerNode TriggerNode::falling(ChannelId const ch) { return makeEdge(ch, EdgeDirection::Falling); }
TriggerNode TriggerNode::anyEdge(ChannelId const ch) { return makeEdge(ch, EdgeDirection::Any); }

TriggerNode TriggerNode::atLevel(ChannelId const ch, LevelState const state) {
    TriggerNode n;
    n.kind = NodeKind::Level;
    n.channel = ch;
    n.level = state;
    return n;
}

TriggerNode TriggerNode::pattern(std::uint64_t const mask, std::uint64_t const expected) {
    TriggerNode n;
    n.kind = NodeKind::Pattern;
    n.patternMask = mask;
    n.patternExpected = expected;
    return n;
}

TriggerNode TriggerNode::pulseWidth(ChannelId const ch, std::int64_t const minTicks,
                                    std::int64_t const maxTicks) {
    TriggerNode n;
    n.kind = NodeKind::PulseWidth;
    n.channel = ch;
    n.thresholdA = minTicks;
    n.thresholdB = maxTicks;
    return n;
}

TriggerNode TriggerNode::count(TriggerNode child, std::int64_t const times) {
    TriggerNode n;
    n.kind = NodeKind::Count;
    n.thresholdA = times;
    n.children.push_back(std::move(child));
    return n;
}

TriggerNode TriggerNode::protocolField(std::string path, ComparisonOp const op,
                                       std::string value) {
    TriggerNode n;
    n.kind = NodeKind::ProtocolField;
    n.fieldPath = std::move(path);
    n.op = op;
    n.compareValue = std::move(value);
    return n;
}

TriggerNode TriggerNode::andAll(std::vector<TriggerNode> nodes) {
    TriggerNode n;
    n.kind = NodeKind::And;
    n.children = std::move(nodes);
    return n;
}

TriggerNode TriggerNode::orAny(std::vector<TriggerNode> nodes) {
    TriggerNode n;
    n.kind = NodeKind::Or;
    n.children = std::move(nodes);
    return n;
}

TriggerNode TriggerNode::notOf(TriggerNode node) {
    TriggerNode n;
    n.kind = NodeKind::Not;
    n.children.push_back(std::move(node));
    return n;
}

bool TriggerNode::isCombinator() const noexcept {
    switch (kind) {
    case NodeKind::And:
    case NodeKind::Or:
    case NodeKind::Not:
    case NodeKind::Sequence:
    case NodeKind::Count:
        return true;
    default:
        return false;
    }
}

std::size_t TriggerNode::nodeCount() const noexcept {
    std::size_t total = 1;
    for (const auto& child : children) {
        total += child.nodeCount();
    }
    return total;
}

std::size_t TriggerNode::depth() const noexcept {
    std::size_t maxChild = 0;
    for (const auto& child : children) {
        maxChild = std::max(maxChild, child.depth());
    }
    return maxChild + 1;
}

// --- validation --------------------------------------------------------------

Status validate(const TriggerNode& node) {
    switch (node.kind) {
    case NodeKind::And:
    case NodeKind::Or:
    case NodeKind::Sequence:
        if (node.children.size() < 2) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("'{}' needs at least 2 children, has {}",
                                             nameOf(node.kind), node.children.size()));
        }
        break;
    case NodeKind::Not:
    case NodeKind::Count:
        if (node.children.size() != 1) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("'{}' needs exactly 1 child, has {}", nameOf(node.kind),
                                             node.children.size()));
        }
        break;
    case NodeKind::Edge:
    case NodeKind::Level:
    case NodeKind::PulseWidth:
    case NodeKind::Timeout:
        if (!node.channel.isValid()) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("'{}' needs a valid channel", nameOf(node.kind)));
        }
        break;
    case NodeKind::Pattern:
        if (node.patternMask == 0) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "pattern trigger has an empty care mask");
        }
        if ((node.patternExpected & ~node.patternMask) != 0) {
            return Status::error(
                ErrorCode::ConfigurationError,
                fmt::format("pattern expected 0x{:x} has bits set outside the mask 0x{:x}",
                            node.patternExpected, node.patternMask));
        }
        break;
    case NodeKind::ByteSequence:
        if (node.bytePattern.empty()) {
            return Status::error(ErrorCode::ConfigurationError,
                                 "byte-sequence trigger has an empty pattern");
        }
        if (!node.byteMask.empty() && node.byteMask.size() != node.bytePattern.size()) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("byte-sequence mask size {} differs from pattern size {}",
                                             node.byteMask.size(), node.bytePattern.size()));
        }
        break;
    case NodeKind::ProtocolField:
    case NodeKind::SearchExpr:
        if (node.fieldPath.empty()) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("'{}' needs a field path", nameOf(node.kind)));
        }
        break;
    case NodeKind::Always:
    case NodeKind::Never:
        break;
    }

    if (node.kind == NodeKind::PulseWidth && node.thresholdA > node.thresholdB) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("pulse-width min {} exceeds max {}", node.thresholdA,
                                         node.thresholdB));
    }
    if (node.kind == NodeKind::Count && node.thresholdA < 1) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("count trigger needs at least 1 occurrence, got {}",
                                         node.thresholdA));
    }
    if (node.children.size() > 64) {
        // A bound so a hostile or buggy workspace file cannot make validation or
        // compilation recurse without limit.
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("'{}' has {} children, the limit is 64", nameOf(node.kind),
                                         node.children.size()));
    }
    for (const auto& child : node.children) {
        auto const st = validate(child);
        if (!st.ok()) {
            return st;
        }
    }
    return Status::success();
}

// --- classification ----------------------------------------------------------

Executability classify(const TriggerNode& node, TriggerCapability const caps) {
    switch (node.kind) {
    case NodeKind::Edge:
        if (node.edge == EdgeDirection::Rising &&
            hasCapability(caps, TriggerCapability::RisingEdge)) {
            return Executability::DeviceAndHost;
        }
        if (node.edge == EdgeDirection::Falling &&
            hasCapability(caps, TriggerCapability::FallingEdge)) {
            return Executability::DeviceAndHost;
        }
        if (node.edge == EdgeDirection::Any && hasCapability(caps, TriggerCapability::AnyEdge)) {
            return Executability::DeviceAndHost;
        }
        return Executability::Unsupported;

    case NodeKind::Level:
        return hasCapability(caps, TriggerCapability::Level) ? Executability::DeviceAndHost
                                                             : Executability::Unsupported;
    case NodeKind::Pattern:
        return hasCapability(caps, TriggerCapability::DigitalPattern)
                   ? Executability::DeviceAndHost
                   : Executability::Unsupported;
    case NodeKind::PulseWidth:
        return hasCapability(caps, TriggerCapability::PulseWidth) ? Executability::DeviceAndHost
                                                                  : Executability::Unsupported;
    case NodeKind::Timeout:
        return hasCapability(caps, TriggerCapability::Timeout) ? Executability::DeviceAndHost
                                                               : Executability::Unsupported;
    case NodeKind::ByteSequence:
        // A byte-level pattern match is a shift-register compare: device-executable
        // WITHOUT protocol semantics. This is the recommended way to express the
        // "MOSI == 0x9F AND MISO == 0xEF" style trigger from master spec section 30.
        return hasCapability(caps, TriggerCapability::ByteSequence) ? Executability::DeviceAndHost
                                                                    : Executability::Unsupported;
    case NodeKind::Count:
        return hasCapability(caps, TriggerCapability::Count) ? Executability::DeviceAndHost
                                                             : Executability::HostOnly;
    case NodeKind::ProtocolField:
    case NodeKind::SearchExpr:
        // Requires protocol semantics, and the firmware deliberately does no
        // protocol decoding (deviation D5). Always host-only.
        return Executability::HostOnly;

    case NodeKind::And:
        return hasCapability(caps, TriggerCapability::And) ? Executability::DeviceAndHost
                                                           : Executability::HostOnly;
    case NodeKind::Or:
        return hasCapability(caps, TriggerCapability::Or) ? Executability::DeviceAndHost
                                                          : Executability::HostOnly;
    case NodeKind::Not:
        return hasCapability(caps, TriggerCapability::Not) ? Executability::DeviceAndHost
                                                           : Executability::HostOnly;
    case NodeKind::Sequence:
        return hasCapability(caps, TriggerCapability::Sequential) ? Executability::DeviceAndHost
                                                                  : Executability::HostOnly;
    case NodeKind::Always:
    case NodeKind::Never:
        return Executability::DeviceAndHost;
    }
    return Executability::Unsupported;
}

namespace {
void classifyRecursive(const TriggerNode& node, TriggerCapability const caps,
                       ClassificationResult& out, Executability& worst) {
    auto const exec = classify(node, caps);
    switch (exec) {
    case Executability::DeviceAndHost:
        out.deviceNodeCount += 1;
        break;
    case Executability::HostOnly:
        out.hostOnlyNodeCount += 1;
        if (worst == Executability::DeviceAndHost) {
            worst = Executability::HostOnly;
            out.reason = fmt::format("'{}' cannot run on the device: it needs protocol semantics "
                                     "or a capability the device did not report",
                                     nameOf(node.kind));
        }
        break;
    case Executability::Unsupported:
        worst = Executability::Unsupported;
        out.reason = fmt::format("'{}' is not supported by this device at all", nameOf(node.kind));
        break;
    }
    for (const auto& child : node.children) {
        classifyRecursive(child, caps, out, worst);
    }
}
}  // namespace

ClassificationResult classifyTree(const TriggerNode& root, TriggerCapability const caps) {
    ClassificationResult out;
    auto worst = Executability::DeviceAndHost;
    classifyRecursive(root, caps, out, worst);
    out.executability = worst;
    if (worst == Executability::DeviceAndHost) {
        out.reason.clear();
    }
    return out;
}

// --- JSON --------------------------------------------------------------------

namespace {

using Json = nlohmann::json;

Json nodeToJson(const TriggerNode& node) {
    Json j;
    j["kind"] = std::string(nameOf(node.kind));
    if (!node.id.empty()) {
        j["id"] = node.id;
    }
    if (node.channel.isValid()) {
        j["channel"] = node.channel.value;
    }
    if (node.kind == NodeKind::Edge) {
        j["edge"] = static_cast<int>(node.edge);
    }
    if (node.kind == NodeKind::Level) {
        j["level"] = static_cast<int>(node.level);
    }
    if (node.kind == NodeKind::Pattern) {
        j["mask"] = node.patternMask;
        j["expected"] = node.patternExpected;
    }
    if (node.kind == NodeKind::PulseWidth || node.kind == NodeKind::Timeout ||
        node.kind == NodeKind::Count) {
        j["thresholdA"] = node.thresholdA;
        if (node.kind == NodeKind::PulseWidth) {
            j["thresholdB"] = node.thresholdB;
        }
    }
    if (node.kind == NodeKind::ByteSequence) {
        j["bytePattern"] = node.bytePattern;
        if (!node.byteMask.empty()) {
            j["byteMask"] = node.byteMask;
        }
    }
    if (node.kind == NodeKind::ProtocolField || node.kind == NodeKind::SearchExpr) {
        j["fieldPath"] = node.fieldPath;
        j["op"] = std::string(nameOf(node.op));
        j["value"] = node.compareValue;
    }
    if (!node.children.empty()) {
        Json arr = Json::array();
        for (const auto& child : node.children) {
            arr.push_back(nodeToJson(child));
        }
        j["children"] = std::move(arr);
    }
    return j;
}

ComparisonOp opFromName(std::string_view const name, bool& ok) {
    for (auto const o : {ComparisonOp::Eq, ComparisonOp::Ne, ComparisonOp::Lt, ComparisonOp::Le,
                         ComparisonOp::Gt, ComparisonOp::Ge, ComparisonOp::In,
                         ComparisonOp::Contains, ComparisonOp::Matches}) {
        if (nameOf(o) == name) {
            ok = true;
            return o;
        }
    }
    ok = false;
    return ComparisonOp::Eq;
}

StatusOr<TriggerNode> nodeFromJson(const Json& j, int depth) {
    if (depth > 32) {
        return Status::error(ErrorCode::ConfigurationError,
                             "trigger tree deeper than 32 levels; refusing to parse");
    }
    if (!j.is_object() || !j.contains("kind") || !j["kind"].is_string()) {
        return Status::error(ErrorCode::ConfigurationError, "trigger node needs a string 'kind'");
    }
    auto kindOr = nodeKindFromName(j["kind"].get<std::string>());
    if (!kindOr.ok()) {
        return kindOr.status();
    }
    TriggerNode node;
    node.kind = *kindOr;
    if (j.contains("id") && j["id"].is_string()) {
        node.id = j["id"].get<std::string>();
    }
    if (j.contains("channel") && j["channel"].is_number_unsigned()) {
        node.channel = ChannelId(static_cast<std::uint16_t>(j["channel"].get<unsigned>()));
    }
    if (j.contains("edge") && j["edge"].is_number_integer()) {
        node.edge = static_cast<EdgeDirection>(j["edge"].get<int>());
    }
    if (j.contains("level") && j["level"].is_number_integer()) {
        node.level = static_cast<LevelState>(j["level"].get<int>());
    }
    if (j.contains("mask") && j["mask"].is_number_unsigned()) {
        node.patternMask = j["mask"].get<std::uint64_t>();
    }
    if (j.contains("expected") && j["expected"].is_number_unsigned()) {
        node.patternExpected = j["expected"].get<std::uint64_t>();
    }
    if (j.contains("thresholdA") && j["thresholdA"].is_number_integer()) {
        node.thresholdA = j["thresholdA"].get<std::int64_t>();
    }
    if (j.contains("thresholdB") && j["thresholdB"].is_number_integer()) {
        node.thresholdB = j["thresholdB"].get<std::int64_t>();
    }
    if (j.contains("bytePattern") && j["bytePattern"].is_array()) {
        node.bytePattern = j["bytePattern"].get<std::vector<std::uint8_t>>();
    }
    if (j.contains("byteMask") && j["byteMask"].is_array()) {
        node.byteMask = j["byteMask"].get<std::vector<std::uint8_t>>();
    }
    if (j.contains("fieldPath") && j["fieldPath"].is_string()) {
        node.fieldPath = j["fieldPath"].get<std::string>();
    }
    if (j.contains("op") && j["op"].is_string()) {
        bool ok = false;
        node.op = opFromName(j["op"].get<std::string>(), ok);
        if (!ok) {
            return Status::error(ErrorCode::ConfigurationError,
                                 fmt::format("unknown comparison operator '{}'",
                                             j["op"].get<std::string>()));
        }
    }
    if (j.contains("value") && j["value"].is_string()) {
        node.compareValue = j["value"].get<std::string>();
    }
    if (j.contains("children")) {
        if (!j["children"].is_array()) {
            return Status::error(ErrorCode::ConfigurationError, "'children' must be an array");
        }
        for (const auto& child : j["children"]) {
            auto parsed = nodeFromJson(child, depth + 1);
            if (!parsed.ok()) {
                return parsed.status();
            }
            node.children.push_back(std::move(*parsed));
        }
    }
    return node;
}

}  // namespace

StatusOr<std::string> toJson(const TriggerNode& node) {
    return nodeToJson(node).dump(2);
}

StatusOr<TriggerNode> fromJson(std::string_view const json) {
    Json parsed;
    try {
        parsed = Json::parse(json);
    } catch (const std::exception& e) {
        return Status::error(ErrorCode::ConfigurationError,
                             fmt::format("trigger JSON parse failed: {}", e.what()));
    }
    return nodeFromJson(parsed, 0);
}

}  // namespace usn::trigger
