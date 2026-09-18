#ifndef RISCVSIM_OPCODES_H
#define RISCVSIM_OPCODES_H

#define NUM_INT_REG 32

/* 指令低 7 位 opcode；二进制注释便于对照手册，常量保持兼容 C11。 */
#define OPCODE_MASK     0x7fu /* 0b1111111 */
#define OPCODE_LUI      0x37u /* 0b0110111 */
#define OPCODE_AUIPC    0x17u /* 0b0010111 */
#define OPCODE_JAL      0x6fu /* 0b1101111 */
#define OPCODE_JALR     0x67u /* 0b1100111 */
#define OPCODE_BRANCH   0x63u /* 0b1100011 */
#define OPCODE_LOAD     0x03u /* 0b0000011 */
#define OPCODE_STORE    0x23u /* 0b0100011 */
#define OPCODE_AMO      0x2fu /* 0b0101111 */
#define OPCODE_OP_IMM   0x13u /* 0b0010011 */
#define OPCODE_OP       0x33u /* 0b0110011 */
#define OPCODE_MISC_MEM 0x0fu /* 0b0001111 */
#define OPCODE_SYSTEM   0x73u /* 0b1110011 */

#endif
