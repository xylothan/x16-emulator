/*

						Extracted from original single fake6502.c file

*/

//
//          65C02 changes.
//
//          ind         absolute indirect
//
//                      A 6502 has a bug whereby if you jmp ($12FF) it reads the address from
//                      $12FF and $1200. This has been fixed in the 65C02.
//
static void imp() { //implied
    // The 65C02's one-cycle NOPs ($x3, $xB) make no second access. WAI and STP
    // are not in the bus traces and are left as they were.
    if (!regs.is65c816 && (opcode & 0x07) == 0x03) {
        return;
    }
    if (opcode == 0xCB || opcode == 0xDB) {
        return;
    }
    pc_dummy(regs.pc);
}

static void imp8() { // brk / cop
    // The signature byte is fetched and discarded.
    pc_dummy(regs.pc);
}

static void acc() { //accumulator
    addressing_is_acc = true;
    pc_dummy(regs.pc);
}

// JSR abs. The 65C02 fetches the high byte after the pushes, in jsr().
static void jsra() {
    ea = fetch6502(regs.pc++, regs.k);
    if (regs.is65c816) {
        ea |= (uint32_t)fetch6502(regs.pc, regs.k) << 8;
        pc_dummy(regs.pc);
        regs.pc++;
    }
}

// JSL. The bank byte is fetched after the program bank is pushed, in jsl().
static void jsla() {
    ea = fetch_word(regs.pc, regs.k);
    regs.pc += 2;
}

// JSR (abs,X). The high byte is fetched after the pushes, in jsx().
static void jsrx() {
    ea = fetch6502(regs.pc++, regs.k);
}

static void imm8() { //immediate, 8bit
    ea = addr_with_k(regs.pc++);
    operand_is_immediate = true;
}

static void immm() { //immediate, 16bit if M = 0
    ea = addr_with_k(regs.pc++);
    operand_is_immediate = true;

    if (memory_16bit()) {
        regs.pc++;
    }
}

static void immx() { //immediate, 16bit if X = 0
    ea = addr_with_k(regs.pc++);
    operand_is_immediate = true;

    if (index_16bit()) {
        regs.pc++;
    }
}

static void imm16() {
    ea = addr_with_k(regs.pc);
    operand_is_immediate = true;
    regs.pc += 2;
}

static void _zp_with_offset(uint16_t offset, bool indexed) {
    uint16_t imm_value = (uint16_t) fetch6502((uint16_t)regs.pc++, regs.k);

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }
    dp_low_dummy();
    if (indexed) {
        dp_index_dummy(imm_value);
    }

    ea = direct_page_add(imm_value + offset);
}

static void _zp_long_with_offset(uint16_t offset) {
    uint16_t eahelp;
    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);
    dp_low_dummy();

    ea = read_long(direct_page_add(eahelp), direct_page_add(eahelp + 1), direct_page_add(eahelp + 2), 0);
    ea = mask_long_addr(ea + offset);

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }
}

static void zp() { //zero-page
    _zp_with_offset(0, false);
}

static void zpx() { //zero-page,X
    _zp_with_offset(regs.x, true);
}

static void zpy() { //zero-page,Y
    _zp_with_offset(regs.y, true);
}

static void rel() { //relative for branch ops (8-bit immediate value, sign-extended)
    reladdr = (uint16_t)fetch6502(regs.pc++, regs.k);
    if (reladdr & 0x80) reladdr |= 0xFF00;
}

static void rel16() { //relative for PER and BRL (16-bit immediate value)
    reladdr = fetch_word(regs.pc, regs.k);
    pc_dummy((uint16_t)(regs.pc + 1));
    regs.pc += 2;
}

static void abso() { //absolute
    ea = addr_with_db(fetch_word(regs.pc, regs.k));
    regs.pc += 2;
}

static void absl() { // absolute long
    ea = fetch_long(regs.pc, regs.k);
    regs.pc += 3;
}

static void absx() { //absolute,X
    uint32_t startpage;
    ea = addr_with_db(fetch_word(regs.pc, regs.k));
    startpage = ea & 0xFFFF00;
    ea = mask_long_addr(ea + regs.x);

    // Compared across the whole 24-bit address: indexing that carries into
    // the next bank costs the same cycle as crossing a page inside one. With
    // 16-bit index registers the cycle is charged whether or not it crosses,
    // which is the datasheet's "add 1 cycle for indexing across page
    // boundaries, or write, or X=0".
    if (index_16bit() || startpage != (ea & 0xFFFF00)) {
        penaltyaddr = 1;
    }
    index_dummy(startpage, (uint16_t)(regs.pc + 1));

    regs.pc += 2;
}

static void abslx() { // absolute long, X
    ea = fetch_long(regs.pc, regs.k);
    ea = mask_long_addr(ea + regs.x);

    // No page-crossing penalty: a long address already names the bank, so
    // indexing across a page costs nothing. Measured flat at five cycles.

    regs.pc += 3;
}

static void absy() { //absolute,Y
    uint32_t startpage;
    ea = addr_with_db(fetch_word(regs.pc, regs.k));
    startpage = ea & 0xFFFF00;
    ea = mask_long_addr(ea + regs.y);

    // Compared across the whole 24-bit address: indexing that carries into
    // the next bank costs the same cycle as crossing a page inside one. With
    // 16-bit index registers the cycle is charged whether or not it crosses,
    // which is the datasheet's "add 1 cycle for indexing across page
    // boundaries, or write, or X=0".
    if (index_16bit() || startpage != (ea & 0xFFFF00)) {
        penaltyaddr = 1;
    }
    index_dummy(startpage, (uint16_t)(regs.pc + 1));

    regs.pc += 2;
}

static void ind() { //indirect - used for jmp, which assumes the pointer is in bank 0!
    uint16_t eahelp, eahelp2;
    eahelp = fetch_word(regs.pc, regs.k);
    //
    //      The 6502 page boundary wraparound bug does not occur on a 65C02.
    //
    //eahelp2 = (eahelp & 0xFF00) | ((eahelp + 1) & 0x00FF); //replicate 6502 page-boundary wraparound bug
    eahelp2 = (eahelp+1) & 0xFFFF;
    ea = read_pair(eahelp, eahelp2, 0);
    // The 65C02 spends a sixth cycle re-reading the pointer's high byte.
    if (!regs.is65c816) {
        dummy6502(eahelp2, 0, false, 0);
    }
    regs.pc += 2;
}

static void aindl() { // [addr]    uint16_t eahelp, eahelp2;
    uint16_t eahelp, eahelp2;
    eahelp = fetch_word(regs.pc, regs.k);
    eahelp2 = (eahelp+1) & 0xFFFF;
    ea = read_long(eahelp, eahelp2, (eahelp2 + 1) & 0xFFFF, 0);
    regs.pc += 2;
}

static void ind0() { // (zp)
    uint16_t eahelp;
    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);
    dp_low_dummy();
    // The pointer lives in the direct page, which is always bank zero. The
    // address it holds is in the data bank.
    ea = addr_with_db(read_pair(direct_page_add(eahelp), direct_page_add(eahelp + 1), 0));

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }
}

static void indl0() { // [dp]
    _zp_long_with_offset(0);
}

static void indx() { // (indirect,X)
    uint16_t eahelp;
    const uint16_t operand = (uint16_t)fetch6502(regs.pc++, regs.k);
    dp_low_dummy();
    dp_index_dummy(operand);
    eahelp = operand + regs.x;
    ea = addr_with_db(read_pair(direct_page_add(eahelp), direct_page_add(eahelp + 1), 0));

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }
}

static void indy() { // (indirect),Y
    uint16_t eahelp;
    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);
    dp_low_dummy();
    uint16_t pointer = read_pair(direct_page_add(eahelp), direct_page_add(eahelp + 1), 0);
    // Indexing applies to the whole 24-bit address, so carrying past $FFFF
    // moves into the next bank rather than wrapping inside this one, and the
    // crossing costs the same cycle as one inside a bank.
    uint32_t startpage = addr_with_db(pointer) & 0xFFFF00;
    ea = mask_long_addr(addr_with_db(pointer) + regs.y);

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }

    if (index_16bit() || startpage != (ea & 0xFFFF00)) {
        penaltyaddr = 1;
    }
    index_dummy(startpage, (uint16_t)(regs.pc - 1));
}

static void indly() { // [dp],Y
    _zp_long_with_offset(regs.y);
}

static void ind0p() { // (zp) used by PEI, which doesn't do wraparound calculations
    uint16_t eahelp;
    eahelp = (uint16_t)fetch6502(regs.pc++, regs.k);
    dp_low_dummy();
    ea = read_pair(regs.dp + eahelp, regs.dp + eahelp + 1, 0);

    if (regs.dp & 0x00FF) {
        penaltyd = 1;
    }
}

static void zprel() { // zero-page, relative for branch ops (8-bit immediatel value, sign-extended) - only used for the 65C02's Rockwell extensions
	// The offset is fetched after the zero-page reads, in bb_bit().
	ea = (uint16_t)fetch6502(regs.pc++, 0);
}

static void sr() { // absolute,S
    // Masked to 16 bits: S can put the stack anywhere inside bank zero, but not
    // outside it, so an offset running past $FFFF wraps rather than reaching
    // bank one.
    ea = (uint16_t)(regs.sp + (uint16_t)fetch6502(regs.pc++, regs.k));
    pc_dummy((uint16_t)(regs.pc - 1));
}

static void sridy() { // (indirect,S),Y
    uint16_t eahelp;
    eahelp = regs.sp + (uint16_t)fetch6502(regs.pc++, regs.k);
    pc_dummy((uint16_t)(regs.pc - 1));
    uint16_t pointer = read_pair(eahelp, eahelp + 1, 0);
    dummy6502((uint16_t)(eahelp + 1), 0, false, 0);
    ea = mask_long_addr(addr_with_db(pointer) + (uint16_t)regs.y);

    // No page-crossing penalty: measured flat at seven cycles.
}

static void bmv() { // block move
    uint8_t dest = fetch6502(regs.pc++, regs.k);
    ea = (fetch6502(regs.pc++, regs.k) << 8) | dest;
}
