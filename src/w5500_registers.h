/**
 * @file w5500_registers.h
 * @brief W5500 register map, SPI block-select encoding and bit constants.
 *
 * Internal to the driver (not installed for users). Derived from the
 * W5500 datasheet and the register layout used by WIZnet ioLibrary.
 */
#ifndef W5500_REGISTERS_H
#define W5500_REGISTERS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* SPI frame: address phase (16 bit) + control phase                  */
/* control = (BSB << 3) | (RWB << 2) | OM                             */
/* ------------------------------------------------------------------ */

#define W5500_SPI_RWB_READ      1u
#define W5500_SPI_RWB_WRITE     0u
#define W5500_SPI_OM_VDM        0u  /* variable length data mode */

#define W5500_BSB_COMMON        0x00u
#define W5500_BSB_SREG(n)       ((uint8_t)(1u + 4u * (n))) /**< socket n registers */
#define W5500_BSB_TXBUF(n)      ((uint8_t)(2u + 4u * (n))) /**< socket n TX buffer */
#define W5500_BSB_RXBUF(n)      ((uint8_t)(3u + 4u * (n))) /**< socket n RX buffer */

/* ------------------------------------------------------------------ */
/* Common register block                                              */
/* ------------------------------------------------------------------ */

#define W5500_REG_MR            0x0000u  /* Mode */
#define W5500_REG_GAR           0x0001u  /* Gateway IP, 4 bytes */
#define W5500_REG_SUBR          0x0005u  /* Subnet mask, 4 bytes */
#define W5500_REG_SHAR          0x0009u  /* Source MAC, 6 bytes */
#define W5500_REG_SIPR          0x000Fu  /* Source IP, 4 bytes */
#define W5500_REG_INTLEVEL      0x0013u
#define W5500_REG_IR            0x0015u
#define W5500_REG_IMR           0x0016u
#define W5500_REG_SIR           0x0017u
#define W5500_REG_SIMR          0x0018u
#define W5500_REG_RTR           0x0019u
#define W5500_REG_RCR           0x001Bu
#define W5500_REG_PHYCFGR       0x002Eu
#define W5500_REG_VERSIONR      0x0039u

/* MR bits */
#define W5500_MR_RST            0x80u
#define W5500_MR_WOL            0x20u
#define W5500_MR_PB             0x10u
#define W5500_MR_PPPOE          0x08u
#define W5500_MR_FARP           0x02u

/* IR bits */
#define W5500_IR_CONFLICT       0x80u
#define W5500_IR_UNREACH        0x40u
#define W5500_IR_PPPOE          0x20u
#define W5500_IR_MP             0x10u

/* PHYCFGR bits */
#define W5500_PHYCFGR_LNK_ON    0x01u
#define W5500_PHYCFGR_DPX_FULL  0x04u
#define W5500_PHYCFGR_SPD_100   0x02u

/* ------------------------------------------------------------------ */
/* Socket register block (offsets within W5500_BSB_SREG(n))            */
/* ------------------------------------------------------------------ */

#define W5500_Sn_MR             0x0000u
#define W5500_Sn_CR             0x0001u
#define W5500_Sn_IR             0x0002u
#define W5500_Sn_SR             0x0003u
#define W5500_Sn_PORT           0x0004u  /* 2 bytes, big endian */
#define W5500_Sn_DHAR           0x0006u  /* 6 bytes */
#define W5500_Sn_DIPR           0x000Cu  /* 4 bytes */
#define W5500_Sn_DPORT          0x0010u  /* 2 bytes */
#define W5500_Sn_MSSR           0x0012u
#define W5500_Sn_TOS            0x0015u
#define W5500_Sn_TTL            0x0016u
#define W5500_Sn_RXBUF_SIZE     0x001Eu
#define W5500_Sn_TXBUF_SIZE     0x001Fu
#define W5500_Sn_TX_FSR         0x0020u  /* 2 bytes */
#define W5500_Sn_TX_RD          0x0022u
#define W5500_Sn_TX_WR          0x0024u
#define W5500_Sn_RX_RSR         0x0026u  /* 2 bytes */
#define W5500_Sn_RX_RD          0x0028u
#define W5500_Sn_RX_WR          0x002Au
#define W5500_Sn_IMR            0x002Cu
#define W5500_Sn_FRAG           0x002Du
#define W5500_Sn_KPALVTR        0x002Fu

/* Sn_MR protocol field */
#define W5500_Sn_MR_CLOSE       0x00u
#define W5500_Sn_MR_TCP         0x01u
#define W5500_Sn_MR_UDP         0x02u
#define W5500_Sn_MR_MACRAW      0x04u
#define W5500_Sn_MR_ND          0x20u  /* no delayed ACK */
#define W5500_Sn_MR_MULTI       0x80u

/* Sn_CR commands */
#define W5500_Sn_CR_OPEN        0x01u
#define W5500_Sn_CR_LISTEN      0x02u
#define W5500_Sn_CR_CONNECT     0x04u
#define W5500_Sn_CR_DISCON      0x08u
#define W5500_Sn_CR_CLOSE       0x10u
#define W5500_Sn_CR_SEND        0x20u
#define W5500_Sn_CR_SEND_MAC    0x21u
#define W5500_Sn_CR_SEND_KEEP   0x22u
#define W5500_Sn_CR_RECV        0x40u

/* Sn_IR bits */
#define W5500_Sn_IR_SENDOK      0x10u
#define W5500_Sn_IR_TIMEOUT     0x08u
#define W5500_Sn_IR_RECV        0x04u
#define W5500_Sn_IR_DISCON      0x02u
#define W5500_Sn_IR_CON         0x01u

/* Sn_SR values */
#define W5500_SOCK_CLOSED       0x00u
#define W5500_SOCK_INIT         0x13u
#define W5500_SOCK_LISTEN       0x14u
#define W5500_SOCK_SYNSENT      0x15u
#define W5500_SOCK_SYNRECV      0x16u
#define W5500_SOCK_ESTABLISHED  0x17u
#define W5500_SOCK_FIN_WAIT     0x18u
#define W5500_SOCK_CLOSING      0x1Au
#define W5500_SOCK_TIME_WAIT    0x1Bu
#define W5500_SOCK_CLOSE_WAIT   0x1Cu
#define W5500_SOCK_LAST_ACK     0x1Du
#define W5500_SOCK_UDP          0x22u
#define W5500_SOCK_MACRAW       0x42u

#endif /* W5500_REGISTERS_H */
