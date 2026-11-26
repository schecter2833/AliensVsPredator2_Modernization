/*
 * thunks.S - one logging thunk per renderer slot.
 *
 * Each thunk saves every register and the flags, reports the slot index,
 * the caller's return address and a pointer to the caller's arguments to
 * avp2m_trace_enter(), restores everything, and then jumps to the stock
 * renderer's function. The jump leaves the stack exactly as the engine set
 * it up, so this works whatever the slot's argument count or return type.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
    .intel_syntax noprefix
    .text

.macro THUNK idx
    .globl _avp2m_thunk_\idx
_avp2m_thunk_\idx:
    pushad                          /* 32 bytes */
    pushfd                          /*  4 bytes -> return address at [esp+36] */
    lea     eax, [esp+40]           /* first stack argument */
    push    eax
    push    \idx
    call    _avp2m_trace_enter      /* void __cdecl (int idx, uint32_t *args) */
    add     esp, 8
    popfd
    popad
    jmp     dword ptr [_avp2m_orig_slots + 4*\idx]
.endm

THUNK 0
THUNK 1
THUNK 2
THUNK 3
THUNK 4
THUNK 5
THUNK 6
THUNK 7
THUNK 8
THUNK 9
THUNK 10
THUNK 11
THUNK 12
THUNK 13
THUNK 14
THUNK 15
THUNK 16
THUNK 17
THUNK 18
THUNK 19
THUNK 20
THUNK 21
THUNK 22
THUNK 23
THUNK 24
THUNK 25
THUNK 26
THUNK 27
THUNK 28
THUNK 29
THUNK 30
THUNK 31
THUNK 32
THUNK 33
THUNK 34
THUNK 35
THUNK 36
THUNK 37

    .section .rdata
    .globl _avp2m_thunk_table
    .align 4
_avp2m_thunk_table:
    .long _avp2m_thunk_0,  _avp2m_thunk_1,  _avp2m_thunk_2,  _avp2m_thunk_3
    .long _avp2m_thunk_4,  _avp2m_thunk_5,  _avp2m_thunk_6,  _avp2m_thunk_7
    .long _avp2m_thunk_8,  _avp2m_thunk_9,  _avp2m_thunk_10, _avp2m_thunk_11
    .long _avp2m_thunk_12, _avp2m_thunk_13, _avp2m_thunk_14, _avp2m_thunk_15
    .long _avp2m_thunk_16, _avp2m_thunk_17, _avp2m_thunk_18, _avp2m_thunk_19
    .long _avp2m_thunk_20, _avp2m_thunk_21, _avp2m_thunk_22, _avp2m_thunk_23
    .long _avp2m_thunk_24, _avp2m_thunk_25, _avp2m_thunk_26, _avp2m_thunk_27
    .long _avp2m_thunk_28, _avp2m_thunk_29, _avp2m_thunk_30, _avp2m_thunk_31
    .long _avp2m_thunk_32, _avp2m_thunk_33, _avp2m_thunk_34, _avp2m_thunk_35
    .long _avp2m_thunk_36, _avp2m_thunk_37
