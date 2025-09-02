#include "cpu/pred/multilevel_btb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/BTB.hh"

namespace gem5::branch_prediction
{

MultiLevelBTB::MultiLevelBTBStats::MultiLevelBTBStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(l1Hits, statistics::units::Count::get(),
               "Number of L1 BTB hits per branch type"),
      ADD_STAT(l2Hits, statistics::units::Count::get(),
               "Number of L2 BTB hits per branch type")
{
    using namespace statistics;

    l1Hits
        .init(enums::Num_BranchType)
        .flags(total | pdf);

    l2Hits
        .init(enums::Num_BranchType)
        .flags(total | pdf);

    for (int i = 0; i < enums::Num_BranchType; i++) {

        l1Hits.subname(i, enums::BranchTypeStrings[i]);
        l2Hits.subname(i, enums::BranchTypeStrings[i]);
    }
}

MultiLevelBTB::MultiLevelBTB(const MultiLevelBTBParams &p)
    : BranchTargetBuffer(p),
      l1btb("l1BTB", p.l1NumEntries, p.l1Associativity,
            p.l1ReplPolicy, p.l1IndexingPolicy, BTBEntry(genTagExtractor(p.l1IndexingPolicy))),
      l2btb("l2BTB", p.l2NumEntries, p.l2Associativity,
            p.l2ReplPolicy, p.l2IndexingPolicy,
            BTBEntry(genTagExtractor(p.l2IndexingPolicy))),
      l1Latency(p.l1Latency),
      l2Latency(p.l2Latency),
      multilevelstats(this)
{
    DPRINTF(BTB, "MultiLevelBTB: Creating L1(%d entries, %d cycles) + L2(%d entries, %d cycles)\n",
            p.l1NumEntries, p.l1Latency, p.l2NumEntries, p.l2Latency);

    if (!isPowerOf2(p.l1NumEntries) || !isPowerOf2(p.l2NumEntries)) {
        fatal("BTB entries must be power of 2!");
    }
}

void
MultiLevelBTB::memInvalidate()
{
    l1btb.clear();
    l2btb.clear();
}

BTBEntry *
MultiLevelBTB::findL1Entry(Addr instPC, ThreadID tid)
{
    return l1btb.findEntry({instPC, tid});
}

BTBEntry *
MultiLevelBTB::findL2Entry(Addr instPC, ThreadID tid)
{
    return l2btb.findEntry({instPC, tid});
}

bool
MultiLevelBTB::valid(ThreadID tid, Addr instPC)
{
    BTBEntry *l1_entry = l1btb.findEntry({instPC, tid});
    if (l1_entry != nullptr) {
        DPRINTF(BTB, "L1 BTB valid for PC %#x\n", instPC);
        return true;
    }

    BTBEntry *l2_entry = l2btb.findEntry({instPC, tid});
    if(l2_entry != nullptr) {
        DPRINTF(BTB, "L2 BTB valid for PC %#x\n", instPC);
        return true;
    }
    return false;
}

const PCStateBase *
MultiLevelBTB::lookup(ThreadID tid, Addr instPC, BranchType type)
{
    BTBLookupResult result = lookupWithLatency(tid, instPC, type);
    return result.target;
}

BTBLookupResult
MultiLevelBTB::lookupWithLatency(ThreadID tid, Addr instPC, BranchType type)
{
    stats.lookups[type]++;

    // lookup l1 btb firstly
    BTBEntry *l1_entry = l1btb.accessEntry({instPC, tid});
    if (l1_entry != nullptr) {
        multilevelstats.l1Hits[type]++;
        DPRINTF(BTB, "L1 BTB hit for PC %#x, latency=%d cycles\n", instPC, l1Latency);
        return BTBLookupResult(l1_entry->target.get(), l1Latency, true, false);
    }

    // L1miss, lookup l2 btb
    BTBEntry *l2_entry = l2btb.accessEntry({instPC, tid});
    if (l2_entry != nullptr) {
        multilevelstats.l2Hits[type]++;
        auto l1_victim = l1btb.findVictim({instPC, tid});
        l1btb.insertEntry({instPC, tid}, l1_victim);
        l1_victim->update(*l2_entry->target, l2_entry->inst);
        DPRINTF(BTB, "L2 BTB hit for PC %#x, latency=%d cycles, insert in L1\n", instPC, l2Latency);
        return BTBLookupResult(l2_entry->target.get(), l2Latency, false, true);
    }
    // Miss in both l1 and l2
    stats.misses[type]++;
    DPRINTF(BTB, "BTB miss for PC %#x\n", instPC);
    return BTBLookupResult(nullptr, l1Latency + l2Latency, false, false);
}

const StaticInstPtr
MultiLevelBTB::getInst(ThreadID tid, Addr instPC)
{
    //For this implementation, L2 is not strictly inclusive of L1, so we need to check both
    BTBEntry *l1_entry = l1btb.findEntry({instPC, tid});
    if (l1_entry) {
        return l1_entry->inst;
    }
    BTBEntry *l2_entry = l2btb.findEntry({instPC, tid});
    if (l2_entry) {
        return l2_entry->inst;
    }
    return nullptr;
}

void
MultiLevelBTB::update(ThreadID tid, Addr instPC,
                      const PCStateBase &target,
                      BranchType type, StaticInstPtr inst)
{
    stats.updates[type]++;


    BTBEntry *l2_victim = l2btb.findVictim({instPC, tid});
    l2btb.insertEntry({instPC, tid}, l2_victim);
    l2_victim->update(target, inst);


    BTBEntry *l1_victim = l1btb.findVictim({instPC, tid});
    l1btb.insertEntry({instPC, tid}, l1_victim);
    l1_victim->update(target, inst);

    DPRINTF(BTB, "Updated BTB for PC %#x -> %#x\n", instPC, target.instAddr());
}


} // namespace gem5::branch_prediction
