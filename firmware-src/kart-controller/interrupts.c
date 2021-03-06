/* 
 * File:   interrupts.c
 * Author: Joshith
 *
 * Created on 23 March, 2015, 11:13 AM
 */

/******************************************************************************/
/* Files to Include                                                           */
/******************************************************************************/

#if defined(__XC)
    #include <xc.h>         /* XC8 General Include File */
#elif defined(HI_TECH_C)
    #include <htc.h>        /* HiTech General Include File */
#endif

#include <stdint.h>         /* For uint8_t definition */
#include <stdbool.h>        /* For true/false definition */

#include "user.h"
#include "nrf24.h"

volatile uint8_t devId  = 0x0;
volatile uint8_t comp  = 0x0;
volatile uint8_t cfgMode= 0x0;
volatile uint8_t tmr0IntCount = 0x0;
//volatile uint8_t ShouldSend = 0;
volatile uint8_t ir_cmd_valid = false;
volatile uint8_t ir_cur_code  = 0;
volatile uint8_t ir_state_pos = 0;
volatile uint8_t hall_detect = false;
volatile gim_timeval ir_cur_hit;
volatile gim_timeval hall_cur_hit;
volatile uint8_t ir_lap_count;
volatile uint8_t hall_lap_count;
volatile uint16_t jiffies = 0x0; /// Jiffies are the Timebase (TMR1)Interrupt count
volatile kart_data_t data[MAX_PAYLOAD_COUNT]; // Payload list to send
volatile int8_t      data_count = 0;
volatile uint8_t     data_valid_bitmap = 0;
volatile uint8_t data_rx_slot       = 0;
volatile uint8_t data_next_rx_slot  = 1;

volatile uint8_t     battery_level = 0xff;
uint8_t  IR_state = true;       //IR Pin State

//unsigned short tmr0IntCount = 0x0;

/* Handle switch event
 * 1. On Feedback Led
 * 2. Check Switch event
 *  a. Long press > 5Sec (Enter Device pgm mode)
 *  b. Short press < 1Sec(program device id)
 */
void handleSwitch() {
    unsigned int cfgModeCounter = 0x0; // inc counter every 10Ms, 500 = 5Sec
    INTCONbits.T0IE = 0;

    if (cfgMode) {
        /* If in programming mode, increment devId */

        IND_LED = 0;
        devId++;
        while (!KEY_1) {
            __delay_ms(20);
        };

        __delay_ms(200);
        IND_LED = 1;
        /* Restart Timer1 for 5Sec*/
        //TMR1RESTART (0,0);
        tmr0IntCount = 1;
        
        TMR0 = 0;
        INTCONbits.T0IF = 0;
        INTCONbits.T0IE = 1;
    } else {
        do {
            if (!cfgMode && (cfgModeCounter >= 250) ) {
                cfgMode = 1; // Enter config mode
                devId = 0; // reset the device Id
                IND_LED = 1;
            } else {
                cfgModeCounter++;
                __delay_ms(20);
            }

            if (cfgMode) {
                /* Start the 5Sec timer0 to unset config mode*/
                tmr0IntCount = 1;
                TMR0 = 0;
                INTCONbits.T0IF = 0;
                INTCONbits.T0IE = 1;
            }
        } while (!KEY_1);
    }
    //IND_LED = 0;        // turn OFF led     
    return;
}

/* The Fixed Frame beacon format uses Modified NEC code */
/* |---4MsPulse(1)---|---2.5MsGap(0)--|--562uS(1)--|--562uS(0)--|--562uS(1)--|--1682uS(0)--|--562uS(1)--|--GAP4.5Ms|*/
/* |--------------Header--------------|----------1--------------|------------0-------------|--Trailer---|*/
/* Proto ladder:
 *  ppos = 0 Reset
 *  ppos = 1 4Ms pulse
 *  ppos = 2 2.5 Ms gap
 *  ppos = 3 562 pulse
 *  ppos = 4 562 gap
 *  ppos = 5 562 pulse
 *  ppos = 6 1.68 gap
 */

/* ******* Apparently, TSOP is active Low. Pulse is infact, low ***************/
#if 1
void
handleIRsignal(void) {
    static uint8_t rangehi, rangelo, ir_cmd;
    uint8_t tstamp = TMR0; // Get time stamp

    //ir_signal_valid = 1;
    if (tstamp < rangelo) {
        /* ERROR: Premature Signal. Wrong packet formatting, reset */
        goto pktstart; //could be a start of new Pkt.
    }

    switch (ir_state_pos) {
        case 1: //4Ms Pulse over, Now wait for 2.5Ms Gap
            rangehi = TMR2mS5HI;
            rangelo = TMR2mS5LO;
            break;
        case 2: //2.5Ms Gap Over, Now data bits. Wait for 562uS pulse for first bit
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 3: // 562uS gap for 1 || 1.6mS for 0
        case 5:
            rangehi = TMR1mS6HI;       // Set Hi for largest possible signal
            rangelo = TMR1mS6LO562;
            break;
        case 4: // See what have we received as first bit in 2 bit code
        case 6:            
            if (tstamp > TMR1mS6LO) // Signal 1. There is a small window of no man's land. However wouldn't be of much worry now.
                ir_cmd = ((ir_cmd <<1) | 1);  // MSb first
            else 
                ir_cmd = ((ir_cmd <<1) | 0);
            // 562uS Pulse for next bit
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 7: // valid packet found, Beacon sensed. 
            rangelo = rangehi = 0;
            ir_state_pos = 0;

            ir_cmd_valid   = true;
            ir_cur_hit.sec = jiffies; // Record Time
            ir_cur_hit.m_sec = (TMR1H - 0xB); // Adjust preset
            ir_cur_code      = ir_cmd;
            
            INTCONbits.T0IE = 0; //Stop TMR0 INT
            return;
        default:
            pktstart:
            // Resets, Try to see header presense
            ir_state_pos = 0;
            ir_cmd       = 0;
            if (SENSOR_IR == 0) { //Yes, this could be start of pkt
                rangehi = TMR4mSHI;
                rangelo = TMR4mSLO;
                INTCONbits.T0IE = 1; //Start of TMR0
            } else { // Nothing useful. Clear everything
                rangelo = rangehi = 0;
                INTCONbits.T0IE = 0; //Stop TMR0 INT
                return;
            }
            break;
    }
    
    INTCONbits.T0IF = 0;
    INTCONbits.T0IE = 1; //Start of TMR0
    TMR0 = rangehi; //Restart with new Hi
    ir_state_pos++; //UP state-machine, if a valid state was detected.    
}
#else 
void
handleIRsignal(void) {
    static uint8_t rangehi, rangelo;
    uint8_t tstamp = TMR0; // Get time stamp

    //ir_signal_valid = 1;
    if (tstamp < rangelo) {
        /* ERROR: Premature Signal. Wrong packet formatting, reset */
        goto pktstart; //could be a start of new Pkt.
    }

    switch (ir_state_pos) {
        case 1: //4Ms Pulse over, Now wait for 2.5Ms Gap
            rangehi = TMR2mS5HI;
            rangelo = TMR2mS5LO;
            break;
        case 2: //2.5Ms Gap Over, Now data[10]. Wait for 562uS pulse for 1
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 3: // 562uS gap for 1
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 4: // 562uS Pulse for 0
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 5: //1.6Ms Gap for 0
            rangehi = TMR1mS6HI;
            rangelo = TMR1mS6LO;
            break;
        case 6: //562uS pulse. Complete packet.
            rangehi = TMR562uSHI;
            rangelo = TMR562uSLO;
            break;
        case 7: // valid packet found, Beacon sensed. 
            rangelo = rangehi = 0;
            ir_state_pos = 0;

            ir_cmd_valid = true;
            ir_cur_hit.sec = jiffies; // Record Time
            ir_cur_hit.m_sec = (TMR1H - 0xB); // Adjust preset

            INTCONbits.T0IE = 0; //Stop TMR0 INT
            return;
        default:
            pktstart :
            // Resets, Try to see header presense
            ir_state_pos = 0;
            if (SENSOR_IR == 0) { //Yes, this could be start of pkt
                rangehi = TMR4mSHI;
                rangelo = TMR4mSLO;
            } else { // Nothing useful. Clear everything
                rangelo = rangehi = 0;
                INTCONbits.T0IE = 0; //Stop TMR0 INT
                return;
            }
            break;
    }

    TMR0 = rangehi; //Restart with new Hi
    INTCONbits.T0IF = 0;
    INTCONbits.T0IE = 1;
    ir_state_pos++; //UP state-machine, if a valid state was detected.    
}
#endif
#if 0
void
handle_hall_cmd(void) {
    
    hall_cur_hit.sec    = jiffies;
    hall_cur_hit.m_sec  = (TMR1H - 0xB); // Adjust preset;
    data[data_rx_slot].battery_level = battery_level;
    data[data_rx_slot].time = hall_cur_hit;
    data[data_rx_slot].dev_id = devId;
    data[data_rx_slot].detect_type = HALL;
    data[data_rx_slot].detect_code = 0;
    data[data_rx_slot].lap_count = hall_lap_count;
    data_set(data_rx_slot);
    data_rx_slot = data_next_rx_slot;
    hall_lap_count++; // Set lap_count wraparound = 63
    data_count++; //new data to send
}
#endif

void interrupt Isr(void)		
{   
    GIE  = 0;              // Global interrupt disable
    if (INTE && INTF) {
        /* External INT, HALL SENSOR*/
        hall_cur_hit.sec = jiffies;
        hall_cur_hit.m_sec = (TMR1H - 0xB); // Adjust preset;
        data[data_rx_slot].battery_level = battery_level;
        data[data_rx_slot].time = hall_cur_hit;
        data[data_rx_slot].dev_id = devId;
        data[data_rx_slot].detect_type = HALL;
        data[data_rx_slot].detect_code = 0;
        data[data_rx_slot].lap_count = hall_lap_count;
        data_set(data_rx_slot);
        data_rx_slot = data_next_rx_slot;
        hall_lap_count++; // Set lap_count wraparound = 63
        data_count++; //new data to send
        //handle_hall_cmd();
        /* Hall processing Done */
        hall_detect = true;
        INTF        = false;
    } else if (INTCONbits.RABIE && INTCONbits.RABIF)	{	// check the interrupt on change flag
        INTCONbits.RABIE = 0;
        if(SENSOR_IR^IR_state) {        //IR Pin State changed?
            IR_state = SENSOR_IR;
            handleIRsignal();
        } else if(!KEY_1) {
            __delay_ms(50); //check for key debounce
            //IND_LED ^=1;                // Toggle the LED
            handleSwitch();
            //ShouldSend = 1;             //Test Send on Keydown
		} else if (KEY_1) {
            __delay_ms(50); //check for key debounce and ignore Key UP sequence
        }
       
		if(PORTA || PORTB) {asm("nop");}             //this is requited to end the mismatch condition 
        INTCONbits.RABIF = 0;                // clear the interrupt on chage flag
        INTCONbits.RABIE = 1;
	} else if (PIR1bits.TMR1IF) {
        TMR1ON      = 0;        // Turn OFF TMR1
        TMR1H       = 0x0B;     // write 0xBDB so that INT is exact 500mS
        TMR1L       = 0xDB;
        jiffies++;         // System timer Tick. 4MHz, 1:8 PS.
        PIR1bits.TMR1IF = 0; // clear flag  
        TMR1ON          = 1; //Restart 
    } else if (INTCONbits.T0IE && INTCONbits.T0IF) { /* TMR0 Config */
        //Fosc:4Mhz;
        //OPTION_REGbits.PS   = 6 (128);
        // Tick => 4/(4*64) = 64uS
        // Overflow Time = 64*255 = 16.32Ms. 255Int = ~4Sec
        if (!cfgMode) { //TMR0 used for IR statemachine, expired. Reset StateMachine
            ir_state_pos = 0;
            INTCONbits.T0IE = 0; //           -->> Turn OFF TIMER1
            //PIR1bits.TMR1IF = 0; // clear flag 
            //return;
        } else if (tmr0IntCount) { // ~4sec  Sec. = 255 ints (init as 1 from key (handleSwitch))
            tmr0IntCount++;     
        } else {
            INTCONbits.T0IE = 0; //           -->> OFF TIMER1
            IND_LED = 0; // Turn OFF LED
            /* Test */
            //IND_LED ^=1;            
            /* End Test */
            cfgMode = 0;
            /* COnfig timeout. Save the config devInt to EEPROM */
            eeprom_write(0x00, (devId & 0xF));
            //comp = (devId << 4);
            //comp = (comp) | ((~devId)&0x0F);
            /* blink the Leds DevID numbers to give feedback of programmed devId*/
            __delay_ms(500);
            blinkLed(devId);
            //comp = (devId << 4)|((~devId)&0x0F);
            //comp = comp|((~devId)&0x0F);
            //GIE  = 1;            // Global interrup enable
        }
        INTCONbits.T0IF = 0;
    }
    
    GIE  = 1;            //Done ISR. Global interrup enable

}
