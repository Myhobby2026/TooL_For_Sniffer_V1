// -----------------------------------------------------------------------------
// trigger_node.h -- a trigger is DATA, not code
// (docs/architecture_review.md section 12).
//
// One serializable AST, compiled to two back-ends: a restricted device subset and
// a full host evaluator. That is what lets the same trigger definition arm the
// hardware AND be re-evaluated against a stored capture, and what lets a future
// trigger graph grow without changing the type.
//
// classify() tells the caller, per node and BEFORE a capture is armed, whether the
// device can honour it -- so the GUI never offers a trigger the hardware cannot
// deliver (master spec section 10).
// -----------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usn/common/status.h"
#include "usn/model/capabilities.h"
#include "usn/model/channel.h"

namespace usn::trigger {

enum class NodeKind : std::uint16_t {
    // leaves
    Edge = 0,
    Level,
    Pattern,
    PulseWidth,
    Timeout,
    ByteSequence,
    ProtocolField,   // host only: needs protocol semantics
    Count,
    SearchExpr,      // host only
    // combinators
    And,
    Or,
    Not,
    Sequence,
    // meta
    Always,
    Never
};

[[nodiscard]] std::string_view nameOf(NodeKind kind) noexcept;
[[nodiscard]] StatusOr<NodeKind> nodeKindFromName(std::string_view name);

enum class EdgeDirection : std::uint8_t { Rising = 0, Falling, Any };
enum class LevelState : std::uint8_t { High = 0, Low };
enum class ComparisonOp : std::uint8_t {
    Eq = 0, Ne, Lt, Le, Gt, Ge, In, Contains, Matches
};

[[nodiscard]] std::string_view nameOf(ComparisonOp op) noexcept;

// Whether a node can run on the device, only on the host, or not at all.
enum class Executability : std::uint8_t { DeviceAndHost = 0, HostOnly, Unsupported };

[[nodiscard]] std::string_view nameOf(Executability exec) noexcept;

struct TriggerNode {
    NodeKind kind{NodeKind::Always};
    std::string id;                       // user-visible label

    std::vector<TriggerNode> children;    // combinators, Count, Sequence

    // leaf parameters
    ChannelId channel{};
    EdgeDirection edge{EdgeDirection::Rising};
    LevelState level{LevelState::High};
    std::uint64_t patternMask{0};
    std::uint64_t patternExpected{0};
    std::int64_t thresholdA{0};           // PulseWidth min / Timeout ticks / Count N
    std::int64_t thresholdB{0};           // PulseWidth max
    std::vector<std::uint8_t> bytePattern;
    std::vector<std::uint8_t> byteMask;
    std::string fieldPath;                // ProtocolField / SearchExpr
    ComparisonOp op{ComparisonOp::Eq};
    std::string compareValue;

    // --- construction helpers (keep call sites readable and validated) --------
    [[nodiscard]] static TriggerNode always();
    [[nodiscard]] static TriggerNode rising(ChannelId ch);
    [[nodiscard]] static TriggerNode falling(ChannelId ch);
    [[nodiscard]] static TriggerNode anyEdge(ChannelId ch);
    // Named atLevel rather than level because `level` is also the name of the
    // LevelState member below, and a static factory cannot share a name with it.
    [[nodiscard]] static TriggerNode atLevel(ChannelId ch, LevelState state);
    [[nodiscard]] static TriggerNode pattern(std::uint64_t mask, std::uint64_t expected);
    [[nodiscard]] static TriggerNode pulseWidth(ChannelId ch, std::int64_t minTicks,
                                                std::int64_t maxTicks);
    [[nodiscard]] static TriggerNode count(TriggerNode child, std::int64_t times);
    [[nodiscard]] static TriggerNode protocolField(std::string path, ComparisonOp op,
                                                   std::string value);
    [[nodiscard]] static TriggerNode andAll(std::vector<TriggerNode> nodes);
    [[nodiscard]] static TriggerNode orAny(std::vector<TriggerNode> nodes);
    [[nodiscard]] static TriggerNode notOf(TriggerNode node);

    [[nodiscard]] bool isCombinator() const noexcept;
    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept;
};

// Structural validation: arity, thresholds, mask widths.
[[nodiscard]] Status validate(const TriggerNode& node);

// Per-node classification against reported device capabilities.
[[nodiscard]] Executability classify(const TriggerNode& node, TriggerCapability caps);

// Whole-tree classification: the tree is device-executable only if every node is.
// On HostOnly/Unsupported, `reason` explains which node and why -- that string is
// what the GUI shows, so it must be specific rather than generic.
struct ClassificationResult {
    Executability executability{Executability::DeviceAndHost};
    std::string reason;                 // empty when DeviceAndHost
    std::size_t deviceNodeCount{0};
    std::size_t hostOnlyNodeCount{0};
};
[[nodiscard]] ClassificationResult classifyTree(const TriggerNode& root, TriggerCapability caps);

// JSON round-trip. Serialized triggers are saved in workspaces and in .usn files,
// so they are diffable and reviewable.
[[nodiscard]] StatusOr<std::string> toJson(const TriggerNode& node);
[[nodiscard]] StatusOr<TriggerNode> fromJson(std::string_view json);

}  // namespace usn::trigger
