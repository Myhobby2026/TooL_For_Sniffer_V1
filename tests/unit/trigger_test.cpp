// -----------------------------------------------------------------------------
// trigger_test.cpp -- tests for usn::trigger (TriggerNode AST, Validation, JSON).
// -----------------------------------------------------------------------------
#include <gtest/gtest.h>

#include "usn/model/capabilities.h"
#include "usn/trigger/trigger_node.h"

TEST(TriggerTest, AstConstructionAndValidation) {
    auto edgeNode = usn::trigger::TriggerNode::rising(usn::ChannelId{2});
    EXPECT_EQ(edgeNode.kind, usn::trigger::NodeKind::Edge);
    EXPECT_EQ(edgeNode.channel.value, 2u);
    EXPECT_EQ(edgeNode.edge, usn::trigger::EdgeDirection::Rising);
    EXPECT_TRUE(usn::trigger::validate(edgeNode).ok());

    auto patNode = usn::trigger::TriggerNode::pattern(0x00FF, 0x00AA);
    EXPECT_EQ(patNode.kind, usn::trigger::NodeKind::Pattern);
    EXPECT_TRUE(usn::trigger::validate(patNode).ok());

    auto andNode = usn::trigger::TriggerNode::andAll({edgeNode, patNode});
    EXPECT_EQ(andNode.kind, usn::trigger::NodeKind::And);
    EXPECT_EQ(andNode.nodeCount(), 3u);
    EXPECT_TRUE(usn::trigger::validate(andNode).ok());

    // Empty combinator should fail validation
    auto emptyAnd = usn::trigger::TriggerNode::andAll({});
    EXPECT_FALSE(usn::trigger::validate(emptyAnd).ok());
}

TEST(TriggerTest, CapabilityClassification) {
    auto edgeNode = usn::trigger::TriggerNode::rising(usn::ChannelId{0});

    // Hardware supports RisingEdge
    usn::TriggerCapability caps = usn::TriggerCapability::RisingEdge;
    EXPECT_EQ(usn::trigger::classify(edgeNode, caps), usn::trigger::Executability::DeviceAndHost);

    // ProtocolField is host-only
    auto protoNode = usn::trigger::TriggerNode::protocolField("spi.data", usn::trigger::ComparisonOp::Eq, "0x42");
    EXPECT_EQ(usn::trigger::classify(protoNode, caps), usn::trigger::Executability::HostOnly);

    // Tree with both device and host-only nodes
    auto tree = usn::trigger::TriggerNode::andAll({edgeNode, protoNode});
    auto result = usn::trigger::classifyTree(tree, caps);
    EXPECT_EQ(result.executability, usn::trigger::Executability::HostOnly);
    EXPECT_FALSE(result.reason.empty());
}

TEST(TriggerTest, JsonRoundTrip) {
    auto orig = usn::trigger::TriggerNode::andAll({
        usn::trigger::TriggerNode::rising(usn::ChannelId{1}),
        usn::trigger::TriggerNode::pattern(0xFF, 0x55)
    });

    auto jsonOr = usn::trigger::toJson(orig);
    ASSERT_TRUE(jsonOr.ok());
    EXPECT_FALSE(jsonOr->empty());

    auto restoredOr = usn::trigger::fromJson(*jsonOr);
    ASSERT_TRUE(restoredOr.ok());
    EXPECT_EQ(restoredOr->kind, orig.kind);
    EXPECT_EQ(restoredOr->children.size(), 2u);
    EXPECT_EQ(restoredOr->children[0].kind, usn::trigger::NodeKind::Edge);
    EXPECT_EQ(restoredOr->children[1].kind, usn::trigger::NodeKind::Pattern);
}
