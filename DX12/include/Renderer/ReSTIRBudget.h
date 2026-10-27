#pragma once
#include <cmath>
#include <cstdint>

// Render-thread controller. Only fresh, fence-completed measurements count.
// Stage 4 has no RT samples: periodically probe stage 3 to detect recovery.
struct ReSTIRBudget
{
    bool Observe(uint64_t seq, float milliseconds, float budget, bool measured)
    {
        if(seq==0 || seq<=sequence) return false;
        sequence=seq;
        if(!std::isfinite(budget) || budget<=0) return false;
        if(settle) { --settle; return false; }
        if(stage==4)
        {
            if(++parked<240 && !(primed && ema<budget*.75f)) return false;
            return Change(3);
        }
        if(!measured || !std::isfinite(milliseconds) || milliseconds<0) return false;
        ema=primed ? ema*.9f+milliseconds*.1f : milliseconds;
        primed=true;
        over=ema>budget ? over+1 : 0;
        under=ema<budget*.75f ? under+1 : 0;
        if(over>=30) return Change(stage+1);
        if(stage && under>=120) return Change(stage-1);
        return false;
    }

private:
    bool Change(uint32_t next)
    {
        stage=next; over=under=parked=0;
        // Drain the three in-flight frames and allow the new NRD state to settle.
        settle=30;
        return true;
    }

public:
    uint32_t stage=0, over=0, under=0, settle=0, parked=0;
    uint64_t sequence=0;
    float ema=0;
    bool primed=false;
};
