/*
 *      This file is part of frosted.
 *
 *      frosted is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License version 2, as
 *      published by the Free Software Foundation.
 *
 *
 *      frosted is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with frosted.  If not, see <http://www.gnu.org/licenses/>.
 *
 *      Authors:
 *
 */
 
#include <stdint.h>
#include "frosted.h"
#include "gpio.h"

#include <pico_device.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/syscfg.h>
#include <libopencm3/ethernet/mac.h>
#include <libopencm3/ethernet/phy.h>
#include "libopencm3/cm3/nvic.h"

#define dbg(...)

#define ETH_MAX_FRAME    (1514)
#define ETH_IRQ_PRIO        (1)

/* FIXME */
#define BOARD_PHY_RMII

#define PHY_PHYID1              0x02    /**< PHYS ID 1.                     */
#define PHY_PHYID2              0x03    /**< PHYS ID 2.                     */

/* Some known PHY-identifiers */
#define PHY_KSZ8021_ID    0x00221556
#define PHY_KS8721_ID     0x00221610
#define PHY_DP83848I_ID   0x20005C90
#define PHY_LAN8710A_ID   0x0007C0F1
#define PHY_DM9161_ID     0x0181B8A0
#define PHY_AM79C875_ID   0x00225540
#define PHY_STE101P_ID    0x00061C50

#define BOARD_phy_addr      PHY_LAN8710A_ID


struct dev_eth {
    struct pico_device dev;
    uint32_t rx_prod;
    uint32_t rx_cons;
    uint8_t mac_addr[6];
    uint8_t phy_addr;
};

static struct dev_eth * dev_eth_stm = NULL;
static const uint8_t default_mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};




    /* For STM32F4DIS-BB Discover-Mo board 
       ETH_MDIO --------------> PA2
       ETH_MDC ---------------> PC1

       ETH_RMII_REF_CLK-------> PA1

       ETH_RMII_CRS_DV -------> PA7
       ETH_MII_RX_ER   -------> PB10
       ETH_RMII_RXD0   -------> PC4
       ETH_RMII_RXD1   -------> PC5
       ETH_RMII_TX_EN  -------> PB11
       ETH_RMII_TXD0   -------> PB12
       ETH_RMII_TXD1   -------> PB13

       ETH_nRST_PIN    -------> PE2
       */

/* XXX: Bad idea to hardcode pinmux here ... */
const struct gpio_addr gpio_eth[] = {
    {.base=GPIOA, .pin=GPIO2, .mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // MDIO
    {.base=GPIOC, .pin=GPIO1, .mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // MDC
    {.base=GPIOA, .pin=GPIO1, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII REF CLK
    {.base=GPIOA, .pin=GPIO7, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII CRS DV
    {.base=GPIOB, .pin=GPIO10,.mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXER
    {.base=GPIOC, .pin=GPIO4, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXD0
    {.base=GPIOC, .pin=GPIO5, .mode=GPIO_MODE_AF, .af=GPIO_AF11, .name=NULL},                        // RMII RXD1
    {.base=GPIOB, .pin=GPIO11,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXEN
    {.base=GPIOB, .pin=GPIO12,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXD0
    {.base=GPIOB, .pin=GPIO13,.mode=GPIO_MODE_AF, .optype=GPIO_OTYPE_PP, .af=GPIO_AF11, .name=NULL}, // RMII TXD1
    {.base=GPIOE, .pin=GPIO2, .mode=GPIO_MODE_OUTPUT, .optype=GPIO_OTYPE_PP, .name=NULL, .pullupdown=GPIO_PUPD_PULLUP},            // PHY RESET
};

static uint32_t eth_smi_get_phy_divider(void)
{
    uint32_t hclk = rcc_ahb_frequency;

#ifdef STM32F4
    /* CSR Clock between 150-168 MHz */ 
    if (hclk >= 150000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_102;    
#endif

    /* CSR Clock between 100-150 MHz */ 
    if (hclk >= 100000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_62;

    /* CSR Clock between 60-100 MHz */ 
    if (hclk >= 60000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_42;

    /* CSR Clock between 35-60 MHz */ 
    if (hclk >= 35000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_26;

    /* CSR Clock between 20-35 MHz */
    if (hclk >= 20000000)
        return ETH_MACMIIAR_CR_HCLK_DIV_16;

    dbg("STM32_HCLK below minimum frequency for ETH operations (20MHz)\n");
    return 0;
}

static int8_t find_phy(uint32_t clk_div)
{
    uint32_t phy;

    for (phy = 0; phy < 31; phy++)
    {
        ETH_MACMIIDR = (phy << 6) | clk_div;
        if ( (eth_smi_read(phy, PHY_PHYID1) == (BOARD_phy_addr >> 16)) &&
            ((eth_smi_read(phy, PHY_PHYID2) & 0xFFF0) == (BOARD_phy_addr & 0xFFF0)) )
            return (int8_t)phy;
    }
    /* PHY not detected */
    return -1;
}

static uint8_t temp_rx_buf[ETH_MAX_FRAME];
static uint8_t temp_tx_buf[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf };
static int stm_eth_poll(struct pico_device *dev, int loop_score)
{
    uint32_t rx_len = 0;
    while (eth_rx(temp_rx_buf, &rx_len, ETH_MAX_FRAME))
    {
        pico_stack_recv(&dev_eth_stm->dev, temp_rx_buf, rx_len);
        rx_len = 0;
        loop_score--;
    }
    return loop_score;
}

static int stm_eth_send(struct pico_device *dev, void * buf, int len)
{
    if (eth_tx(buf, len))
        return len;
    else
        return 0;
}

/* link state -> 0 == DOWN, 1 == UP */
static int stm_eth_link_state(struct pico_device *dev)
{
    struct dev_eth *stm = (struct dev_eth *)dev;
    /* test link */
    if (phy_link_isup(stm->phy_addr))
        return 1;
    else
        return 0;
}

/**
 * Description:         Low level MAC initialization.
 * Parameters:  phy_addr    Pointer a phy_addr variable
 */
static int mac_init(uint8_t * phy_addr, uint32_t clk_div)
{
    int8_t phy_detect;

    /* Enable SYSCFG clock */
    rcc_periph_clock_enable(RCC_SYSCFG);

    /* MAC clocks stopped to configure RMII .*/
    rcc_periph_clock_disable(RCC_ETHMAC);
    rcc_periph_clock_disable(RCC_ETHMACTX);
    rcc_periph_clock_disable(RCC_ETHMACRX);

    /* FIXME */
    #define SYSCFG_PMC_MII_RMII_SEL         ((uint32_t)0x00800000) /*!<Ethernet PHY interface selection */
    #if defined(BOARD_PHY_RMII)
      SYSCFG_PMC |= SYSCFG_PMC_MII_RMII_SEL;
    #else
      SYSCFG_PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
    #endif

    /* Enable RCC clocks: Eth, GPIO */
    rcc_periph_clock_enable(RCC_ETHMAC);
    rcc_periph_clock_enable(RCC_ETHMACTX);
    rcc_periph_clock_enable(RCC_ETHMACRX);

    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_GPIOE);

    rcc_osc_on(RCC_PLL);
    rcc_wait_for_osc_ready(RCC_PLL);

    /* Reset of the MAC core.*/
    rcc_periph_reset_pulse(RST_ETHMAC);

    /* MAC clocks temporary activation.*/
    rcc_periph_clock_enable(RCC_ETHMAC);
    rcc_periph_clock_enable(RCC_ETHMACTX);
    rcc_periph_clock_enable(RCC_ETHMACRX);

    /* PHY address setup.*/
    phy_detect = find_phy(clk_div);
    if (phy_detect == -1)   /* Detect PHY address */
        return -1;          /* PHY not found */
    *phy_addr = (uint8_t)phy_detect;

    #define PHY_BMCTRL              0x00    /**< Basic mode control register.   */
    #define BMCTRL_PDOWN            0x0800  /**< Powerdown                      */
    #define BMCTRL_RESET            0x8000  /**< Reset                          */
    /* PHY Soft Reset */
    eth_smi_write(*phy_addr, PHY_BMCTRL, BMCTRL_RESET);
    while (eth_smi_read(*phy_addr, PHY_BMCTRL) & BMCTRL_RESET) {};

    /* ETH DMA Soft Reset */
    ETH_DMABMR |= ETH_DMABMR_SR;
    while(ETH_DMABMR & ETH_DMABMR_SR) {};

    return 0;
}


int stm_eth_init(void)
{
    int8_t phy_addr;
    uint32_t clk_div = eth_smi_get_phy_divider();

    dev_eth_stm = kalloc(sizeof(struct dev_eth));
    if (!dev_eth_stm)
        return -1;

    memset(dev_eth_stm, 0, sizeof(struct dev_eth));

    mac_init(&dev_eth_stm->phy_addr, clk_div);

    uint8_t * descriptors;
    descriptors = kalloc(2 * 2 * ETH_MAX_FRAME);
    if (!descriptors)
        return -1;

    /* try DMA soft-reset */
    ETH_DMABMR |= ETH_DMABMR_SR;
    while(ETH_DMABMR & ETH_DMABMR_SR) {};

    eth_init(phy_addr, clk_div); /* does a phy_reset */
    eth_desc_init(descriptors, 2, 2, ETH_MAX_FRAME, ETH_MAX_FRAME, true);
    eth_set_mac((uint8_t*)default_mac);

    /* set pico function pointers */
    dev_eth_stm->dev.poll = stm_eth_poll;
    dev_eth_stm->dev.send = stm_eth_send;
    dev_eth_stm->dev.link_state = stm_eth_link_state;

    if (pico_device_init(&dev_eth_stm->dev,"eth0", default_mac) < 0) {
        kfree(dev_eth_stm);
        return -1;
    }

    /* Enable interrupt vector .*/
    nvic_set_priority(NVIC_ETH_IRQ, ETH_IRQ_PRIO);
    nvic_enable_irq(NVIC_ETH_IRQ);

    eth_start();

    eth_irq_enable(ETH_DMAIER_NISE | ETH_DMAIER_RIE | ETH_DMAIER_TIE); /* NISE needed ?? */


    /* Enabling required interrupt sources.*/
    ETH_DMASR    = ETH_DMASR;
    ETH_DMAIER   = ETH_DMAIER_NISE | ETH_DMAIER_RIE | ETH_DMAIER_TIE;

    /* DMA general settings.*/
  #define ETH_DMABMR_RDP_1Beat    ((uint32_t)0x00020000)  /* maximum number of beats to be transferred in one RxDMA transaction is 1 */
  #define ETH_DMABMR_PBL_1Beat    ((uint32_t)0x00000100)  /* maximum number of beats to be transferred in one TxDMA (or both) transaction is 1 */
    ETH_DMABMR   = ETH_DMABMR_AAB | ETH_DMABMR_RDP_1Beat | ETH_DMABMR_PBL_1Beat;

    /* Transmit FIFO flush.*/
    ETH_DMAOMR   = ETH_DMAOMR_FTF;
    while (ETH_DMAOMR & ETH_DMAOMR_FTF)
    {};

    /* DMA final configuration and start.*/
    ETH_DMAOMR   = ETH_DMAOMR_DTCEFD | ETH_DMAOMR_RSF | ETH_DMAOMR_TSF | ETH_DMAOMR_ST | ETH_DMAOMR_SR;

    return 0;
}

void eth_isr(void)
{
    uint32_t bits = ETH_DMASR;

    /* Clear all bits */
    ETH_DMASR = bits;

    if (bits & ETH_DMASR_RS)
    {
        /* Receive Status bit set */
        dev_eth_stm->rx_prod++;
    }
}
