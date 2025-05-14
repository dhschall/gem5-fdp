# Copyright (c) 2007-2008 The Hewlett-Packard Development Company
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

microcode = """
def macroop IRET_REAL {
    .serialize_after

    # temp_RIP
    ld t1, ss, [1, t0, rsp], "0 * env.dataSize", addressSize=ssz
    # temp_CS
    ld t2, ss, [1, t0, rsp], "1 * env.dataSize", addressSize=ssz
    # temp_RFLAGS
    ld t3, ss, [1, t0, rsp], "2 * env.dataSize", addressSize=ssz

    # Update RSP now that all memory accesses have succeeded.
    addi rsp, rsp, "3 * env.dataSize", dataSize=ssz

    #@TODO do the pushes this way.

    # Update CS.
    wrsel cs, t2
    # Make sure there isn't any junk in the upper bits of the base.
    mov t4, t0, t2
    slli t4, t4, 4, dataSize=8
    wrbase cs, t4, dataSize=8

    # Update RFLAGS
    # Get the current RFLAGS
    rflags t4, dataSize=8
    # Flip flag bits if they should change.
    mov t5, t4, t3
    xor t5, t5, t4, dataSize=8
    # Don't change VIF, VIP, or VM
    limm t6, "~(VIFBit | VIPBit | VMBit)", dataSize=8
    and t5, t5, t6, dataSize=8
    # Write back RFLAGS with flipped bits.
    wrflags t4, t5, dataSize=8

    # Update RIP
    wrip t1, t0
};

def macroop IRET_PROT {
    .serialize_after
    .adjust_env oszIn64Override

    # Check for a nested task. This isn't supported at the moment.
    rflag t1, 14; #NT bit
    panic "Task switching with iret is unimplemented!", flags=(nCEZF,)

    #t1 = temp_RIP
    #t2 = temp_CS
    #t3 = temp_RFLAGS
    #t4 = handy m5 register

    # Pop temp_RIP, temp_CS, and temp_RFLAGS
    ld t1, ss, [1, t0, rsp], "0 * env.dataSize", addressSize=ssz
    ld t2, ss, [1, t0, rsp], "1 * env.dataSize", addressSize=ssz
    ld t3, ss, [1, t0, rsp], "2 * env.dataSize", addressSize=ssz

    # Read the handy m5 register for use later
    rdm5reg t4


###
### Handle if we're returning to virtual 8086 mode.
###

    #IF ((temp_RFLAGS.VM=1) && (CPL=0) && (LEGACY_MODE))
    #    IRET_FROM_PROTECTED_TO_VIRTUAL

    #temp_RFLAGS.VM != 1
    rcri t0, t3, 18, flags=(ECF,)
    br label("protToVirtFallThrough"), flags=(nCECF,)

    #CPL=0
    andi t0, t4, 0x30, flags=(EZF,)
    br label("protToVirtFallThrough"), flags=(nCEZF,)

    #(LEGACY_MODE)
    rcri t0, t4, 1, flags=(ECF,)
    br label("protToVirtFallThrough"), flags=(nCECF,)

    panic "iret to virtual mode not supported"

protToVirtFallThrough:



    #temp_CPL = temp_CS.rpl
    andi t5, t2, 0x3


###
### Read in the info for the new CS segment.
###

    #CS = READ_DESCRIPTOR (temp_CS, iret_chk)
    andi t0, t2, 0xFC, flags=(EZF,), dataSize=2
    br label("processCSDescriptor"), flags=(CEZF,)
    andi t6, t2, 0xF8, dataSize=8
    andi t0, t2, 0x4, flags=(EZF,), dataSize=2
    br label("globalCSDescriptor"), flags=(CEZF,)
    ld t8, tsl, [1, t0, t6], dataSize=8, addressSize=8, atCPL0=True
    br label("processCSDescriptor")
globalCSDescriptor:
    ld t8, tsg, [1, t0, t6], dataSize=8, addressSize=8, atCPL0=True
processCSDescriptor:
    chks t2, t6, dataSize=8


###
### Get the new stack pointer and stack segment off the old stack if necessary,
### and piggyback on the logic to check the new RIP value.
###
    #IF ((64BIT_MODE) || (temp_CPL!=CPL))
    #{

    #(64BIT_MODE)
    andi t0, t4, 0xE, flags=(EZF,)
    # Since we just found out we're in 64 bit mode, take advantage and
    # do the appropriate RIP checks.
    br label("doPopStackStuffAndCheckRIP"), flags=(CEZF,)

    # Here, we know we're -not- in 64 bit mode, so we should do the
    # appropriate/other RIP checks.
    # if temp_RIP > CS.limit throw #GP(0)
    rdlimit t6, cs, dataSize=8
    sub t0, t6, t1, flags=(ECF,)
    fault "std::make_shared<GeneralProtection>(0)", flags=(CECF,)

    #(temp_CPL!=CPL)
    srli t7, t4, 4
    xor t7, t7, t5
    andi t0, t7, 0x3, flags=(EZF,)
    br label("doPopStackStuff"), flags=(nCEZF,)
    # We can modify user visible state here because we're know
    # we're done with things that can fault.
    addi rsp, rsp, "3 * env.dataSize", dataSize=ssz
    br label("fallThroughPopStackStuff")

doPopStackStuffAndCheckRIP:
    # Check if the RIP is canonical.
    srai t7, t1, 47, flags=(EZF,), dataSize=8
    # if t7 isn't 0 or -1, it wasn't canonical.
    br label("doPopStackStuff"), flags=(CEZF,)
    addi t0, t7, 1, flags=(EZF,), dataSize=8
    fault "std::make_shared<GeneralProtection>(0)", flags=(nCEZF,)

doPopStackStuff:
    #    POP.v temp_RSP
    ld t6, ss, [1, t0, rsp], "3 * env.dataSize", addressSize=ssz
    #    POP.v temp_SS
    ld t9, ss, [1, t0, rsp], "4 * env.dataSize", addressSize=ssz
    #    SS = READ_DESCRIPTOR (temp_SS, ss_chk)
    andi t0, t9, 0xFC, flags=(EZF,), dataSize=2
    br label("processSSDescriptor"), flags=(CEZF,)
    andi t7, t9, 0xF8, dataSize=8
    andi t0, t9, 0x4, flags=(EZF,), dataSize=2
    br label("globalSSDescriptor"), flags=(CEZF,)
    ld t7, tsl, [1, t0, t7], dataSize=8, addressSize=8, atCPL0=True
    br label("processSSDescriptor")
globalSSDescriptor:
    ld t7, tsg, [1, t0, t7], dataSize=8, addressSize=8, atCPL0=True
processSSDescriptor:
    chks t9, t7, dataSize=8

    # This actually updates state which is wrong. It should wait until we know
    # we're not going to fault. Unfortunately, that's hard to do.
    wrdl ss, t7, t9
    wrsel ss, t9

###
### From this point downwards, we can't fault. We can update user visible state.
###
    #    RSP.s = temp_RSP
    mov rsp, rsp, t6, dataSize=ssz

    #}

fallThroughPopStackStuff:

    # Update CS
    wrdl cs, t8, t2
    wrsel cs, t2

    #CPL = temp_CPL

    #IF (changing CPL)
    #{
    srli t7, t4, 4
    xor t7, t7, t5
    andi t0, t7, 0x3, flags=(EZF,)
    br label("skipSegmentSquashing"), flags=(CEZF,)

    # The attribute register needs to keep track of more info before this will
    # work the way it needs to.
    #    FOR (seg = ES, DS, FS, GS)
    #        IF ((seg.attr.dpl < cpl && ((seg.attr.type = 'data')
    #            || (seg.attr.type = 'non-conforming-code')))
    #        {
    #            seg = NULL
    #        }
    #}

skipSegmentSquashing:

    # Ignore this for now.
    #RFLAGS.v = temp_RFLAGS
    wrflags t0, t3
    #  VIF,VIP,IOPL only changed if (old_CPL = 0)
    #  IF only changed if (old_CPL <= old_RFLAGS.IOPL)
    #  VM unchanged
    #  RF cleared

    #RIP = temp_RIP
    wrip t0, t1
};

def macroop IRET_VIRT {
    panic "Virtual mode iret isn't implemented!"
};

def macroop INT3 {

    limm t1, 0x03, dataSize=8

    rdip t7

    # Are we in long mode?
    rdm5reg t5
    andi t0, t5, 0x1, flags=(EZF,)
    br rom_label("longModeSoftInterrupt"), flags=(CEZF,)
    br rom_label("legacyModeInterrupt")
};

def macroop INT3_VIRT {
    panic "Virtual mode int3 isn't implemented!"
};

def macroop INT3_REAL {
    panic "Real mode int3 isn't implemented!"
};

def macroop INT_LONG_I {
    #load the byte-sized interrupt vector specified in the instruction
    .adjust_imm trimImm(8)
    limm t1, imm, dataSize=8

    rdip t7

    # Are we in long mode?
    br rom_label("longModeSoftInterrupt")
};

def macroop INT_PROT_I {
    #load the byte-sized interrupt vector specified in the instruction
    .adjust_imm trimImm(8)
    limm t1, imm, dataSize=8

    rdip t7

    # Are we in long mode?
    br rom_label("legacyModeInterrupt")
};

def macroop INT_REAL_I {
    #load the byte-sized interrupt vector specified in the instruction
    .adjust_imm trimImm(8)
    limm t1, imm, dataSize=8

    # temp_RIP
    ld t2, idtr, [4, t1, t0], dataSize=2, addressSize=8
    # temp_CS
    ld t3, idtr, [4, t1, t0], 2, dataSize=2, addressSize=8

    cda ss, [1, t0, rsp], -2, dataSize=2, addressSize=ssz
    cda ss, [1, t0, rsp], -6, dataSize=2, addressSize=ssz

    rflags t4, dataSize=8
    rdsel t5, cs, dataSize=8
    rdip t6

    # Push RFLAGS.
    st t4, ss, [1, t0, rsp], -2, dataSize=2, addressSize=ssz
    # Push CS.
    st t5, ss, [1, t0, rsp], -4, dataSize=2, addressSize=ssz
    # Push the next RIP.
    st t6, ss, [1, t0, rsp], -6, dataSize=2, addressSize=ssz

    # Update RSP
    subi rsp, rsp, 6, dataSize=ssz

    # Set the CS selector.
    wrsel cs, t3, dataSize=2
    # Make sure there isn't any junk in the upper bits of the base.
    mov t3, t0, t3
    # Compute and set CS base.
    slli t3, t3, 4, dataSize=8
    wrbase cs, t3, dataSize=8

    # If AC, TF, IF or RF are set, we want to flip them.
    limm t7, "(ACBit | TFBit | IFBit | RFBit)", dataSize=8
    and t7, t4, t7, dataSize=8
    wrflags t4, t7, dataSize=8

    # Set the new RIP
    wrip t2, t0
};

def macroop INT_VIRT_I {
    panic "Virtual mode int3 isn't implemented!"
};


def macroop STUI {
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc")
    limm t7, 1<<63,dataSize=8
    or t1, t1, t7, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrMisc"), t1
};
def macroop TESTUI {
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc")
    sexti t1, t1, 63, flags=(CF,)
};
def macroop CLUI {
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc")
    limm t7, ~(1<<63),dataSize=8
    and t1, t1, t7, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrMisc"), t1
};
def macroop UIRET {
    # if PciON=1
    rdval t1, ctrlRegIdx("misc_reg::UintrPciON"),dataSize=8
    andi t0, t1, 1,dataSize=8, flags=(ZF,)
    br label("notpci"), flags=(CZF,)
    trylock:
    rdval t2, ctrlRegIdx("misc_reg::UintrPciLock"),dataSize=8
    subi t0, t2, 2,dataSize=8, flags=(ZF,)
    br label("trylock"), flags=(CZF,)
    wrval ctrlRegIdx("misc_reg::UintrPciLock"), t1,dataSize=8
    rdval t3, ctrlRegIdx("misc_reg::UintrPciPending"),dataSize=8
    rdval t4, ctrlRegIdx("misc_reg::UintrPciConsumed"),dataSize=8
    sub t3, t3, t4, dataSize=8, flags=(ZF,)
    wrval ctrlRegIdx("misc_reg::UintrPciConsumed"), t0, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrPciPending"), t3,dataSize=8
    br label("zero"), flags=(CZF,)
    rdval t4, ctrlRegIdx("misc_reg::UintrPciEarlyExit"),dataSize=8
    subi t4, t4, 1, flags=(ZF,)
    br label("zero"), flags=(CZF,)
    rdval t1, ctrlRegIdx("misc_reg::UintrHandler"),dataSize=8
    rdval t8, ctrlRegIdx("misc_reg::UintrPciRSP"),dataSize=8
    wrip t0, t1,dataSize=8
    mov rsp, rsp, t8, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrPciLock"), t0
    br label("done")
    zero:
    wrval ctrlRegIdx("misc_reg::UintrPciON"), t0,dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrPciLock"), t0,dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrPciEarlyExit"), t0,dataSize=8
    rdval t6, ctrlRegIdx("misc_reg::UintrPciPC"),dataSize=8
    rdval t7, ctrlRegIdx("misc_reg::UintrPciRFLAGS"),dataSize=8
    wrip t6, t0, dataSize=8
    rflags t6, dataSize=8
    limm t1, ~(0x254DD5), dataSize=8
    rdval t8, ctrlRegIdx("misc_reg::UintrPciRSP"),dataSize=8
    and t6, t6, t1, dataSize=8
    limm t1, (0x254DD5), dataSize=8
    mov rsp, rsp, t8, dataSize=8
    and t7, t7, t1, dataSize=8
    or t6, t6, t7, dataSize=8
    wrflags t6, t0, dataSize=8
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc"),dataSize=8
    limm t8,1<<63,dataSize=8
    or t1, t1, t8, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrMisc"), t1,dataSize=8
    br label("done")


    notpci:
    # pop tempRip
    ld t6, ss, [1, t0, rsp], dataSize=8, addressSize=8
    addi rsp, rsp, 8, dataSize=8
    # pop tempRFLAGS
    ld t7, ss, [1, t0, rsp], dataSize=8, addressSize=8
    # track user interrupt overlap! use in delivery or wait till pc ready/first fetch after then go
    addi rsp, rsp, 8, dataSize=8
    # pop tempRsp
    ld t8, ss, [1, t0, rsp], dataSize=8, addressSize=8
    addi rsp, rsp, 8, dataSize=8

    #Rip:= tempRip
    wrip t6, t0, dataSize=8
    #RFLAGS:= (RFLAGS & ~254DD5H) | (tempRFLAGS & 254DD5H)
    rflags t6, dataSize=8
    limm t1, ~(0x254DD5), dataSize=8
    and t6, t6, t1, dataSize=8
    limm t1, (0x254DD5), dataSize=8
    and t7, t7, t1, dataSize=8
    or t6, t6, t7, dataSize=8
    
    
    #Rsp:=tempRsp
    mov rsp, rsp, t8, dataSize=8

    rdval t2, ctrlRegIdx("misc_reg::UintrTimerStatus"),dataSize=8
    # timer_active timer_should_reset timer_on 
    # -----1------ ---------0-------- ----1--- = 5
    limm t3, 5|288, dataSize=8
    sub t0, t2, t3, flags=(ZF,)
    br label("skip_timer_update"), flags=(nCZF,)
    # timer_active timer_should_reset timer_on 
    # -----0------ ---------1-------- ----1--- = 3
    limm t2, 3, dataSize=8
    wrval  ctrlRegIdx("misc_reg::UintrTimerStatus"), t2,dataSize=8
    skip_timer_update:


    #UIF:=1
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc"),dataSize=8
    limm t8,1<<63,dataSize=8
    or t1, t1, t8, dataSize=8
    wrval ctrlRegIdx("misc_reg::UintrMisc"), t1,dataSize=8
    wrflags t6, t0, dataSize=8
    done:
    andi t1, t1, 1


};

def macroop SENDUIPI_R {
    #if reg > UITTSZ
    rdval t1, ctrlRegIdx("misc_reg::UintrMisc")
    limm t3, 0xffffffff, dataSize=8
    and t2, t1, t3, dataSize=8
    sub t3, reg, t2, dataSize=8, flags=(OF, SF, ZF,)
    # throw #GP(O)
    fault "std::make_shared<GeneralProtection>(0)", flags=(nCSxOvZF,)
    rdval t4, ctrlRegIdx("misc_reg::UintrTT")
    #t4<-UITTADDR
    limm t3, "(uint64_t(-(16ULL)))",dataSize=8
    and t4, t4, t3, dataSize=8
    #t4<-UITTADDR+reg<<4
    slli t2, reg, 4, dataSize=8
    add t4, t4, t2
    #tempUITTE in t5, t6
    ld t5, seg, [1, t0, t4], dataSize=8, atCPL0=True
    addi t4, t4, 8
    ld t6, seg, [1, t0, t4], dataSize=8, atCPL0=True
    andi t7, t5, 1, dataSize=1, flags=(ZF,)
    fault "std::make_shared<GeneralProtection>(0)", flags=(CZF,)
    andi t7, t5, 0xFE, dataSize=1, flags=(ZF,)
    fault "std::make_shared<GeneralProtection>(0)", flags=(nCZF,)
    mfence
    #tempUIPD in t1, t2 
    ld t1, seg, [1, t0, t6], dataSize=8, atCPL0=True
    addi t6, t6, 8
    ld t2, seg, [1, t0, t6], dataSize=8, atCPL0=True
    #
    srli t9, t1, 2, dataSize=8
    srli t10, t1, 24, dataSize=8
    andi t10, t10, 0xff, dataSize=8 
    limm t7, 0x3fff, dataSize=8
    and t9, t9, t7, dataSize=8
    or t9, t9, t10, dataSize=8, flags=(ZF,)
    br label("pass"), flags=(CZF,)
    mfence
    fault "std::make_shared<GeneralProtection>(0)"
    pass:

    limm t9, 1, dataSize=8
    #tempUITTE.UV
    srli t3, t5, 8, dataSize=8
    andi t3, t3, 0x3F, dataSize=8
    
    sll t9, t9, t3, dataSize=8
    #tempUPID.PIR[tempUITTE.UV] := 1
    or t2, t2, t9, dataSize=8
    #if tempUPID.SN = tempUPID.ON = 0
    andi t8, t1, 3, dataSize=8, flags=(ZF,)
    br label("send"), flags=(CZF,)
    # Only t2 changed
    st t2, seg, [1, t0, t6], dataSize=8, atCPL0=True
    mfence
    br label("done"), flags=()
    send:
    ori t1, t1, 1
    st t2, seg, [1, t0, t6], dataSize=8, atCPL0=True
    subi t6, t6, 8
    st t1, seg, [1, t0, t6], dataSize=8, atCPL0=True
   # #UITTE is released t5, t6 free
    mfence
    rdval t9, ctrlRegIdx("misc_reg::ApicBase")
    limm t7, (1<<11), dataSize=8 
    and t8, t9, t7, flags=(ZF,)
    br label("done"), flags=(CZF,)
    # WE NEED SET UIRR 167

    limm t7, (1<<10), dataSize=8
    and t8, t9, t7, flags=(ZF,)
    srli t6, t1, 16, dataSize=8
    andi t6, t6, 0xFF, dataSize=8
    #tempUPID.NV in t6 
    srli t5, t1, 32, dataSize=8
    #tempUPID.NDST in t5
    br label("xAPIC"), flags=(CZF,)
    #x2APIC
    panic "x2APIC is not supported yet"
    br label("done"), flags=()
    xAPIC:
    limm t7, 0x0000000FFFFFF000
    and t9, t9, t7, dataSize=8
    limm t7, 0x310, dataSize=8
    add t9, t9, t7, dataSize=8
    srli t5, t5, 8, dataSize=4
    slli t5, t5, 24, dataSize=4
    st t5, seg, [1,t0,t9], dataSize=4, physical=True, uncacheable=True, atCPL0=True
    subi t9, t9, 0x10, dataSize=8
    ld t1, seg, [1,t0,t9], dataSize=4, physical=True, uncacheable=True, atCPL0=True
    limm t7,(1<<12), dataSize=4
    and t1, t1, t7, dataSize=4
    limm t7,1<<14, dataSize=4
    or t1, t1, t7, dataSize=4
    or t6, t6, t1, dataSize=4
    st t6, seg, [1,t0,t9], dataSize=4, physical=True, uncacheable=True, atCPL0=True
    # ld t1, seg, [1, t0, t6], dataSize=8, physical=True, uncacheable=True
    done:
    andi t1, t1, 1
   #panic "no"
};

"""
# let {{
#    class INT(Inst):
#       "GenFault ${new UnimpInstFault}"
#    class INTO(Inst):
#       "GenFault ${new UnimpInstFault}"
# }};
