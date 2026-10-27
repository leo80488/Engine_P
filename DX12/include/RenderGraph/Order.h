#pragma once

// RenderGraph/Order.h - where a pass goes among its stage's passes.
//
// A STAGE (RenderGraph/Stage.h) says which part of the frame a pass belongs to. An ORDER
// places it among that stage's passes when its registration position should not decide:
//   RG::After("CloudPass", "...")   right after CloudPass
//   RG::Before("VideoPass", "...")  right before VideoPass
// given at RenderGraph::AddPass(stage, pass, { ... }) or a render feature's
// ctx.AddPass(pass, { ... }).
//
// Compile sorts the registration order into the declaration (= execution) order once per
// pass-list change: by stage; inside a stage by the constraints, then an anchor (a pass with
// After constraints right after the latest-registered target, one with only Before
// constraints right before the earliest, any other at its registration position), then the
// registration order. Only staged passes move; an Unstaged one (a preview graph's, a harness
// test pass) keeps its position. [RG_ORDER] passes= staged= constraints= moved= inverted=
// cycles= unknown= is the census: moved = the fewest passes that explain the new order (those
// off its longest run that kept the registration order), each logged with its constraints;
// inverted = registrations that went back a stage.
//
// The reason is required: it records what breaks without the order, and the snapshot
// (-rgdump, the `rg` verb) carries it. A name targets every other staged pass with that
// GetName() (an Unstaged one, like a harness duplicate, is never a target). An unknown name, a
// name of no other staged pass, a cycle (that stage falls back to its registration order) and a
// constraint against the stages (After a later stage's pass, Before an earlier one's) are
// reported (ENGINE_CHECK) and dropped; one that agrees with the stages is already satisfied.

#include <cstdint>
#include <string>

namespace RG
{
    struct Order
    {
        enum class Kind : uint8_t { After, Before };
        Kind        kind = Kind::After;
        const char* pass = nullptr;   // the other pass's GetName()
        const char* why  = nullptr;   // what breaks if this order does not hold
    };

    inline Order After(const char* pass, const char* why)  { return { Order::Kind::After, pass, why }; }
    inline Order Before(const char* pass, const char* why) { return { Order::Kind::Before, pass, why }; }

    // The stored form (a graph pass's, a snapshot pass's): the strings copied.
    struct OrderRule
    {
        Order::Kind kind = Order::Kind::After;
        std::string pass;
        std::string why;
    };

    inline const char* OrderKindName(Order::Kind k) { return k == Order::Kind::After ? "After" : "Before"; }
}
