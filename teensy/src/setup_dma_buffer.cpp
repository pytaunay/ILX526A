#include "Arduino.h"
#include "DMAChannel.h"
#include "setup_clk.h"
#include "setup_dma_buffer.h"

DMAChannel dma_portc; // DMA for reading PORTC
DMAChannel dma_buffer_transfer;
DMAChannel dma_enable_send;
extern DMAChannel dma_exposure_cnt_start;

// List of pins mapped to the ADC output
uint8_t const portc_pins[NBIT] = {15,22,23,9,10,13,11,12,28,27,29,30};

volatile uint8_t send_data = 0x00;
volatile uint16_t pix_data[NPIX+100] = {0}; // Padding
volatile uint16_t pix_buffer[NPIX+100] = {0};
volatile uint16_t pix_sum[2*(NPIX+100)] = {0};
volatile uint16_t tmp_data = 0x10;

/* 
 * Function: setup_dma_portc
 * Description: sets the DMA request for transferring data from PORTC to the hold buffer 
 * Poll the register GPIOC_PDIR and transfer its content into the pixel of interest
 * Half of GPIOC_PDIR register is transferred (16 bits) since we care only about the first 12 bits
 */
void setup_dma_portc() {
    // Define all of our inputs
    for(int idx = 0; idx < NBIT; ++idx) {
        pinMode(portc_pins[idx],INPUT);
    }    

    // Enable DMA requests for FTM1, on the rising edge of port 17 (that's the ADC) 
    CORE_PIN17_CONFIG |= PORT_PCR_IRQC(1);

    // Use a single DMA channel triggered on the ADC to grab data from PORTC
    // Source: GPIOC_PDIR -> all of Port C  (12 bits on the Teensy, though the register is 32 bits)
    dma_portc.source(GPIOC_PDIR);
//    dma_portc.source(tmp_data);

    // Size
    dma_portc.transferSize(2); // 2 bytes = 16 bits
    dma_portc.transferCount(1); // Only one transfer 

    // Destination
    // destinationCircular from DMAchannels.h sets up a lot of the DMA transfer registers so we don't have to
    // A circular buffer should work since we do our NPIX + 100 ADC transfers THEN we send the data IMMEDIATELY to the host.
    // There is no risk for data to be overwritten
    // DADDR = &pix_buffer --- address of the first destination
    // DOFF = 2 --- offset in bytes to find the next destination
    // BITER, CITER = 2*(NPIX+100)/2 --- number of major loops to transfer i.e. number of times 2 bytes are transferred
    // NBYTES = 2 --- 2 bytes transferred per major loop count
    // ATTR_DST = (31-clz(NPIX+100)) << 3 | 1 --- the OR operation sets the first three bits of the register: size of the destination location; here 2 bytes 
    // The clz operation is a trick to remove the leading zeros of the modulo value (here NPIX+100). 31 - clz(NPIX+100) sets the bit value of the modulo.
    // It is shifted to the left by three bits so that bits 3 through 7 in the register are set to the closest power of 2 from NPIX+100 and enable the modulo operation.
    // DLAST_SGA = 0 --- circular buffer: once done with each major iteration we use the modulo counter to find the next address 
//    dma_portc.destinationCircular(pix_buffer,2*(NPIX+100));
    dma_portc.TCD->DADDR = &pix_buffer[0];
    dma_portc.TCD->BITER = BUF_SIZE;
    dma_portc.TCD->CITER = BUF_SIZE;
    dma_portc.TCD->DOFF = 2;
    dma_portc.TCD->DLASTSGA = -2*BUF_SIZE;
    dma_portc.TCD->ATTR_DST = 1;


//DMA_TCD1_DADDR = samples;
//  DMA_TCD1_DOFF = 2; // Destination offset (2 byte)
//          DMA_TCD1_DLASTSGA = -sizeof(samples);  // Restore destination address after major loop
//                  DMA_TCD1_ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1); // Source and destination size 16 bit
//                          DMA_TCD1_NBYTES_MLNO = 2; // Number of bytes to transfer (in each service request)
//                                  DMA_TCD1_CITER_ELINKNO = sizeof(samples) / 2; // Set loop counts
//                                      DMA_TCD1_BITER_ELINKNO = sizeof(samples) / 2;
//                                              DMA_TCD1_CSR = DMA_TCD_CSR_INTMAJOR; // Enable interrupt (end-of-major loop)


    // Trigger on falling edge of FTM1
    // Since PORTB only has the ADC clock running, we can trigger on all of port B (how convenient is that?)
    // Having a DMA on the falling edge allows for data to be valid
    dma_portc.triggerAtHardwareEvent(DMAMUX_SOURCE_PORTB);
    dma_portc.enable();
}

/*
 * Function: setup_dma_buffer_transfer
 * Description: enables a DMA transfer that triggers when the SHUT drains closes. 
 * The transfer moves the raw data gathered into pix_buffer into a staging buffer for data transfer.
 * The staging buffer is twice the size of pix_buffer so that it can average two complete samples in the case we try to obtain _very_ short exposure times.
 * Once the transfer is done, an ISR is raised so that averaging can be triggered, and data can be sent away. 
 * If the exposure time is less than 3 ms, then we can start to average to complete samples.
 *
 */
uint8_t trig_send_data = 0x01;
void setup_dma_buffer_transfer() {

    //// TEST DATA
    for(int i = 0;i<BUF_SIZE;++i) {
        //pix_buffer[i] = BUF_SIZE-i;
        //pix_buffer[i] = i;
        pix_buffer[i] = 0;
        pix_data[i] = 0;
    }

    for(int i = 0;i<2*BUF_SIZE;++i)
        pix_sum[i] = 0;

    //// DMA request for the buffer
    dma_buffer_transfer.source(pix_buffer[0]);
    dma_buffer_transfer.destination(pix_sum[0]);

    // Set up 2 transfers of uint16_t array of size NPIX+100
    // Corresponds to 2*2*(NPIX+100) bytes
    dma_buffer_transfer.TCD->NBYTES = 2*BUF_SIZE;
    dma_buffer_transfer.TCD->CITER  = 1;
    dma_buffer_transfer.TCD->BITER  = 1;
    dma_buffer_transfer.TCD->SOFF = 2;
    dma_buffer_transfer.TCD->DOFF = 2;
    dma_buffer_transfer.TCD->ATTR_SRC = 1; 
    dma_buffer_transfer.TCD->ATTR_DST = 1; 
    dma_buffer_transfer.TCD->SLAST = -2*BUF_SIZE;
    dma_buffer_transfer.TCD->DLASTSGA = -2*BUF_SIZE;

   
    // The DMA triggers during the exposure time 
    dma_buffer_transfer.triggerAtCompletionOf(dma_exposure_cnt_start);

    dma_buffer_transfer.enable();

    //// DMA request to trigger the buffer being sent
    dma_enable_send.source(trig_send_data);
    dma_enable_send.destination(send_data);
    dma_enable_send.transferSize(1);
    dma_enable_send.transferCount(1);
    dma_enable_send.triggerAtCompletionOf(dma_buffer_transfer);
    dma_enable_send.enable();

    // The setup below can be used if we have a buffer we fill twice before averaging
   // dma_buffer_transfer.TCD->CITER  = 2;
   // dma_buffer_transfer.TCD->BITER  = 2;
   // dma_buffer_transfer.TCD->DLASTSGA = -4*(NPIX+100);
    // Enables an interrupt at half to reset the source address...
    // There may be a more elegant way to do that
//    dma_buffer_transfer.interruptAtHalf();
//    dma_buffer_transfer.attachInterrupt(isr_buffer_transfer_src_reset);


//    dma_enable_send.interruptAtCompletion();
//    dma_enable_send.attachInterrupt(isr_buffer_transfer);

}


