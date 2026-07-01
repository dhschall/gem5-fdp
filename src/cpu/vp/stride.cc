VPResult
LVPStride::lookup(ThreadID tid, Addr inst_addr, InstSeqNum seq_num)
{
    // Stats
    stats.lookups++;
    //-----

    // Addr idx = index(inst_addr);
    LVPEntry::KeyType key = index(tid, inst_addr);
    LVPEntry *entry = lvpTable.findEntry(key);
    VPResult result;
    result.value = 0;
    result.predict = false;
    result.taken = LVP_STRONG_UNPREDICTABLE;

    if (entry && entry->tid == tid) {

        // Get the number of inflights
        unsigned inflights = numInflights(inst_addr);
        // The value is this instance + in-flights * the stride
        result.value = entry->value + ((inflights + 1) * entry->stride);

        if (entry->confidence >= confThreshold) {

            result.predict = true;
            result.taken = LVP_PREDICTABLE;
        }
        DPRINTF(LVP,
                "VP for iaddr=%#x, lastval=%llu, "
                "stride=%i, inflights=%i, conf=%i\n",
                inst_addr, entry->value, entry->stride, inflights,
                entry->confidence);
        lvpTable.accessEntry(entry);
    }

    // Push the prediction instance to the in-flight queue
    inflightPred.push_front({inst_addr, seq_num});

    DPRINTF(LVP,
            "LVP::%s(iaddr=%#x, sn=%i)"
            "res:[pred=%i, value=%llu, taken=%i] IFsize=%i\n",
            __func__, inst_addr, seq_num, result.predict, result.value,
            result.taken, inflightPred.size());
    return result;
}

void
LVPStride::update(ThreadID tid, Addr inst_addr, InstSeqNum seq_num,
                  Addr load_address, RegVal correct_val, RegVal predicted_val,
                  LVPType classification, Cycles rn_to_ex_delay, bool critical,
                  Cycles exec_time, bool l1Miss)
{

    LVPEntry::KeyType key = index(tid, inst_addr);
    LVPEntry *entry = lvpTable.findEntry(key);

    update_stats(tid, inst_addr, seq_num, load_address, correct_val,
                 predicted_val, classification, rn_to_ex_delay, critical,
                 exec_time, entry, l1Miss);

    DPRINTF(LVP,
            "LVP::%s(iaddr=%#x, sn=%llu, pval=%#x, cval=%#x, class=%i) "
            "pred=%i, correct=%i, delay=%i\n",
            __func__, inst_addr, seq_num, predicted_val, correct_val,
            classification, classification == LVP_PREDICTABLE,
            predicted_val == correct_val, rn_to_ex_delay);

    // First Update
    if (entry == nullptr) {
        entry = lvpTable.findVictim(key);
        lvpTable.insertEntry(key, entry);

        entry->confidence = 0;
        entry->tid = tid;
        entry->stride = 0;
        entry->value = correct_val;
        DPRINTF(LVP, "Allocate new entry: %llu\n", entry->value);

        assert(inflightPred.size());
        assert(inflightPred.back().sn == seq_num);
        inflightPred.pop_back();
        return;
    }

    lvpTable.accessEntry(entry);
    uint64_t last_value = entry->value;
    uint64_t pred_value = entry->value + entry->stride;
    int64_t stride =
        useStride ? ((int64_t)correct_val - (int64_t)last_value) : 0;

    // Update the latest value + stride
    entry->value = correct_val;
    DPRINTF(LVP, "Size: %i\n", inflightPred.size());
    assert(inflightPred.size());
    assert(inflightPred.back().sn == seq_num);
    inflightPred.pop_back();

    if (pred_value != correct_val) {
        if (entry->confidence > 0) {
            entry->confidence = confResetToZero ? 0 : entry->confidence - 1;
        }
        if (entry->confidence == 0) {
            entry->stride = stride;
        }
    } else {
        if (entry->confidence < confThreshold) {
            entry->confidence++;
        }
    }

    DPRINTF(LVP,
            "Entry update: pred=%i, conf=%i, val=%li, "
            "last_val %li, pred_val %li, stride=%li, IFsize=%i\n",
            pred_value != correct_val, entry->confidence, entry->value,
            last_value, pred_value, entry->stride, inflightPred.size());
}
