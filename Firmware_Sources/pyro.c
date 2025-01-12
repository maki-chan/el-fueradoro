/*
 *           Firmware
 *         EL FUERADORO
 *
 *         Felix Pflaum
 *
 */

// Everything to be included is in global.h
#include "global.h"

// Global Variables
static volatile uint8_t  timer1_flags = 0, channel_monitor = 0, key_flag = 0, clear_lcd_tx_flag = 0, clear_lcd_rx_flag = 0;
static volatile uint16_t transmit_flag = 0, hist_del_flag = 0, active_channels = 0;

void wdt_init( void ) {
    MCUSR = 0;
    wdt_disable();
    return;
}

// Initialise Key-Switch
void key_init( void ) {
    KEY_PORT |= ( 1 << KEY );
    KEY_DDR  &= ~( 1 << KEY );

    // Activate Pin-Change Interrupt
    #if ( KEY_NUMERIC == BPORT )
        PCICR  |= ( 1 << PCIE0 );
        PCMSK0 |= ( 1 << KEY );
    #elif ( KEY_NUMERIC == DPORT )
        PCICR  |= ( 1 << PCIE2 );
        PCMSK2 |= ( 1 << KEY );
    #else
        PCICR  |= ( 1 << PCIE1 );
        PCMSK1 |= ( 1 << KEY );
    #endif
    // Keep low-impedance path between ignition voltage and clamps closed
    MOSSWITCHPORT &= ~( 1 << MOSSWITCH );
    MOSSWITCHDDR  |= ( 1 << MOSSWITCH );
}

// Un-initialise Key-Switch (needed only if a device configured as ignition device gets configured as transmitter while on)
void key_deinit( void ) {
    KEY_PORT &= ~( 1 << KEY );
    KEY_DDR  &= ~( 1 << KEY );

    // Deactivate Pin-Change Interrupt
    #if ( KEY_NUMERIC == BPORT )
        PCICR  &= ~( 1 << PCIE0 );
        PCMSK0 &= ~( 1 << KEY );
    #elif ( KEY_NUMERIC == DPORT )
        PCICR  &= ~( 1 << PCIE2 );
        PCMSK2 &= ~( 1 << KEY );
    #else
        PCICR  &= ~( 1 << PCIE1 );
        PCMSK1 &= ~( 1 << KEY );
    #endif
}

// Switch debouncing
uint8_t debounce( volatile uint8_t *port, uint8_t pin ) {
    uint8_t keystate = 0x55, ctr = 0, timer0_regb = TCCR0B;

    // Reset Timer 0
    TCNT0  = 0;
    TIFR0  = ( 1 << TOV0 );
    TCNT0  = 160;
    TCCR0B = ( 1 << CS02 | 1 << CS00 );

    // Query switch state every 10 ms until 8 identical states in a row are received
    while ( ( ( keystate != 0x00 ) && ( keystate != 0xFF ) ) || ( ctr < 8 ) ) {
        keystate <<= 1;                                // Shift left
        keystate  |= ( ( *port & ( 1 << pin ) ) > 0 ); // Write 0 or 1 to LSB

        while ( !( TIFR0 & ( 1 << TOV0 ) ) ) // Wait for timer overflow
            ;

        TIFR0 = ( 1 << TOV0 ); // Clear interrupt flag
        TCNT0 = 160;           // Preload timer

        if ( ctr < 8 ) {
            ctr++;             // Make sure at least 8 queries are executed

        }
    }

    TCCR0B = timer0_regb;

    // return 1 for active switch, 0 for inactive
    return keystate == 0;
}

// Create special symbols for LCD
void create_symbols( void ) {
    // Switch to CGRAM
    lcd_send( 1 << 6, 0 );

    // Symbol "fire"
    lcd_cgwrite( 4 );
    lcd_cgwrite( 4 );
    lcd_cgwrite( 10 );
    lcd_cgwrite( 10 );
    lcd_cgwrite( 21 );
    lcd_cgwrite( 17 );
    lcd_cgwrite( 10 );
    lcd_cgwrite( 4 );

    // Symbol "°"
    lcd_cgwrite( 2 );
    lcd_cgwrite( 5 );
    lcd_cgwrite( 2 );
    lcd_cgwrite( 0 );
    lcd_cgwrite( 0 );
    lcd_cgwrite( 0 );
    lcd_cgwrite( 0 );
    lcd_cgwrite( 0 );

    // Switch back to DDRAM
    lcd_send( 1 << 7, 0 );
}

// ------------------------------------------------------------------------------------------------------------------------

// Talking to radio modules
uint8_t asciihex( char inp ) {
    inp |= 0x20;

    if ( ( inp >= '0' ) && ( inp <= '9' ) ) {
        return inp - '0';
    }

    if ( ( inp >= 'a' ) && ( inp <= 'f' ) ) {
        return inp - 'a' + 10;
    }

    return 255;
}

uint16_t rfmtalk( void ) {
    char          inp;
    unsigned char uinp, error;
    uint16_t      rfm_order = 0;

    error = 0;

    #if ( RFM == 69 )
        uart_puts( "\n\rKommando (16-Bit-Hex-Format, MSB zu 1 setzen für Schreibzugriff): 0x" );
    #else
        uart_puts( "\n\rKommando (16-Bit-Hex-Format): 0x" );
    #endif

    for ( uint8_t i = 4; i; i-- ) {
        rfm_order <<= 4;
        inp         = uart_getc();
        uart_putc( inp );
        uinp       = asciihex( inp );
        error     += ( uinp == 255 );
        rfm_order |= uinp;
    }

    if ( !error ) {
        return rfm_order;
    }
    else {
        return 0xFFFF;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

// Temperature sensor detection
uint8_t tempident( void ) {
    uint8_t checkup = 0;

    for ( uint8_t i = 5; i; i-- ) {
        checkup += w1_reset();                         // In case of sensor connection w1_reset() will return 0
        checkup += !( ( W1_PIN & ( 1 << W1 ) ) && 1 ); // In case of sensor connection the right side will end up as 0
    }

    // Type of temp. sensor is detected by return value
    if ( !checkup ) {
        return DS18B20;
    }
    else {
        W1_DDR  &= ~( 1 << W1 );
        W1_PORT |= ( 1 << W1 );
        return 0;
    }
}

// Temperature measurement
int8_t tempmeas( uint8_t type ) {
    int16_t  temperature = -128;
    uint16_t temp_hex;
    uint32_t utimer;

    if ( !type ) {
        return -128;
    }
    else {
        w1_temp_conf( 125, -40, 9 );
        w1_command( CONVERT_T, NULL );

        utimer = F_CPU / 128;

        while ( --utimer && !w1_bit_io( 1 ) );

        w1_command( READ, NULL );
        temp_hex    = w1_byte_rd();
        temp_hex   += ( w1_byte_rd() ) << 8;
        temperature = w1_tempread_to_celsius( temp_hex, 0 );

        return (int8_t)temperature;
    }
}

// ------------------------------------------------------------------------------------------------------------------------

// Check if received uart-data are a valid ignition command
uint8_t fire_command_uart_valid( const char *field ) {
    return ( field[0] == 0xFF ) && ( field[1] > 0 ) && ( field[1] < (MAX_ID+1) ) && ( field[2] > 0 ) && ( field[2] < 17 )
           && ( field[3] == crc8( crc8( 0, field[1] ), field[2] ) );
}

// ------------------------------------------------------------------------------------------------------------------------

void device_initialisation( uint8_t ignotrans ) {
    // Initialisation for transmitter
    if ( !ignotrans ) {
        clear_lcd_rx_flag = 0;
        clear_lcd_tx_flag = 0;
        hist_del_flag     = 0;
        key_deinit();
        adc_deinit();
        key_flag = 0;
        leds_off();
        lcd_init();
        create_symbols();
        lcd_cursorset( 2, 1 );
        lcd_send( 0, 1 );
        lcd_send( 0, 1 );
        lcd_puts( "  El Fueradoro  " );
        lcd_send( 0, 1 );
        lcd_send( 0, 1 );
        lcd_cursorhome();
        TCCR0B |= ( 1 << CS02 | 1 << CS00 );
        TIMSK0 |= ( 1 << TOIE0 );
    }
    // Initialisation for ignition device
    else {
        sr_init();
        key_init();
        key_flag = 1;
        adc_init();
        leds_off();
    }
}

// ------------------------------------------------------------------------------------------------------------------------

// Shift LCD-cursor (line 3 and 4)
void cursor_x_shift( uint8_t *last_zeile, uint8_t *last_spalte, uint8_t *anz_zeile, uint8_t *anz_spalte ) {
    uint8_t lastzeile = *last_zeile, lastspalte = *last_spalte, anzzeile = *anz_zeile,
            anzspalte = *anz_spalte;

    lastzeile  = anzzeile;
    lastspalte = anzspalte;
    anzspalte += 7;

    if ( anzspalte > 21 ) {
        anzzeile++;
        anzspalte = 1;
    }

    if ( anzzeile > 4 ) {
        anzzeile = 3;
    }

    *last_zeile = lastzeile, *last_spalte = lastspalte, *anz_zeile = anzzeile, *anz_spalte = anzspalte;
}

// Main programme
int main( void ) {
    wdt_disable();
    MOSSWITCHPORT &= ~( 1 << MOSSWITCH );
    MOSSWITCHDDR  |= ( 1 << MOSSWITCH );

    // Local Variables
    uint16_t scheme = 0, anti_scheme = 0, controlvar;
    uint8_t  i, nr, inp, tmp;
    uint8_t  tx_length = 2, rx_length = 0;
    uint8_t  temp_sreg;
    uint8_t  slave_id = MAX_ID, unique_id = MAX_ID, rem_sid = MAX_ID, rem_uid = MAX_ID;
    uint8_t  rfm_rx_error = 1, rfm_tx_error = 0;
    uint8_t  loopcount = 5, transmission_allowed = 1;
    uint8_t  anzspalte = 1, anzzeile = 3, lastspalte = 15, lastzeile = 4;
    uint8_t  armed       = 0;
    uint8_t  changes     = 0;
    uint8_t  iderrors    = 0;
    uint8_t  rssi        = 0;
    uint8_t  ledscheme   = 0;
    int8_t   temperature = -128;

    bitfeld_t flags;
    flags.complete = 0;

    char        uart_field[MAX_COM_ARRAYSIZE + 2] = { 0 };
    char        rx_field[MAX_COM_ARRAYSIZE + 1]   = { 0 };
    char        tx_field[MAX_COM_ARRAYSIZE + 1]   = { 0 };
    char        quantity[MAX_ID + 1]              = { 0 };
    fireslave_t slaves[MAX_ID + 1];
    char        lcd_array[MAX_COM_ARRAYSIZE + 1] = { 0 };
    uint8_t     channel_timeout[16]              = { 0 };


    /* For security reasons the shift registers are initialised right at the beginning to guarantee a low level at the
     * gate pins of the MOSFETs and beware them from conducting directly after turning on the device.
     *
     * The SIPO-registers 74HC595 providing the 16 channels for ignition internally possess a shift register and
     * an output register. Into the latter data from the shift register are transferred by a rising edge at the RCLOCK-pin.
     * This leads to the situation that changes within the shift register are only visible at the outputs after a rising
     * edge at the RCLOCK-pin.
     *
     * As soon as the PIN /OE (Inverted output enable) is pulled low, the outputs are enabled. The problem is that the initial
     * output states are random and cannot be predicted nor controlled. The available "Master Reset" does only affect the internal
     * shift register but has no influence on the outputs unless a rising edge at RCLOCK happens.
     *
     * To guarantee a defined power-on-state the /OE-pin needs to be pulled high until all 16 channels are set to low and
     * written into the output register. Therefore an external pullup-resistor is connected to /OE which makes the outputs tri-state.
     * Additional pulldown-resistor-arrays pull the shift register outputs to "low". Only after 16 zeros have been clocked into the
     * shift register and its values have been written to the output registers /OE may be set to "low".
     *
     * Therefore it is strcitly recommended not to change sr_init() and to call sr_init() immediately as the first function right at
     * the beginning of the main-programme. sr_init() takes care of a safe start for the ignition devices and doesn't do any harm to
     * the transmitter.
     *
     */
    sr_init();

    // Disable unused controller parts (2-wire-Interface, Timer 2, Analogue Comparator)
    PRR  |= ( 1 << PRTWI ) | ( 1 << PRTIM2 );
    ACSR |= 1 << ACD;

    // Initialise Timer
    timer1_init();
    timer1_on();

    // Initialise LEDs
    led_init();

    // Initialise ADC and find out what kind of device the software runs on
    adc_init();
    const uint8_t ig_or_notrans = ( adc_read( 5 ) < 156 );

    // Get Slave- und Unique-ID from EEPROM for ignition devices
    update_addresses( &unique_id, &slave_id );

    if ( ig_or_notrans ) {
        if ( !unique_id || !slave_id ) {
            if ( !unique_id ) {
                unique_id = MAX_ID;
            }

            if ( !slave_id ) {
                slave_id = MAX_ID;
            }

            addresses_save( unique_id, slave_id );
            wdt_enable( 6 );

            while ( 1 );
        }
    }
    else {
        if ( unique_id || slave_id ) {
            addresses_save( 0, 0 );
            wdt_enable( 6 );

            while ( 1 );
        }
    }

    // Initialise arrays
    for ( uint8_t warten = 0; warten < MAX_COM_ARRAYSIZE; warten++ ) {
        uart_field[warten]             = 1;
        tx_field[warten]               = 0;
        rx_field[warten]               = 0;
    }

    for ( uint8_t warten = 0; warten < MAX_ID; warten++ ) {
        slaves[warten].slave_id        = 0;
        slaves[warten].battery_voltage = 0;
        slaves[warten].sharpness       = 0;
        slaves[warten].temperature     = -128;
        slaves[warten].rssi            = 0;
    }

    // Initialise devices
    device_initialisation( ig_or_notrans );

    // Initialise UART and tell the PC we're ready
    uart_init( BAUD );
    allow_uart_sending();

    // Detect temperature sensor and measure temperature if possible
    const uint8_t tempsenstype = tempident();
    temperature = tempmeas( tempsenstype );

    // Initialise radio
    rfm_init();

    // Set encryption active, read and transfer AES-Key
    rfm_cmd( 0x3DA1, 1 );
    for ( uint8_t i = 0; i < 16; i++ ) {
        rfm_cmd( ( 0x3E00 + i * ( 0x0100 ) ) | eeread( START_ADDRESS_AESKEY_STORAGE + i ), 1 );
    }

    #if ( RFM == 69 )
        uint8_t rfm_pwr = eeread( RFM_PWR_ADDRESS );

        if ( ( eeread( RFM_PWR_ADDRESS + 1 ) == crc8( 0x11, rfm_pwr ) ) && ( rfm_pwr < 0x20 ) ) {
            rfm_cmd( ( 0x1180 | rfm_pwr ), 1 );
        }

    #endif

    if ( TRANSMITTER ) {
        tx_field[0] = IDENT;
        tx_field[1] = 'd';
        tx_field[2] = '0';

        // Transmit something to make other devices adjust to frequency
        for ( uint8_t j = 5; j; j-- ) {
            led_green_on();
            rfm_transmit( "UUUUUUUUU\x01", 10 );
            led_green_off();
            _delay_ms( 200 );
        }

        lcd_clear();
        TIMSK0 |= ( 1 << TOIE0 );
    }
    else {
        // Display slave ID
        leds_off();
        _delay_ms(150);
        led_yellow_on();
        _delay_ms(150);
        led_green_on();
        _delay_ms(150);
        led_orange_on();
        _delay_ms(150);
        led_red_on();
        _delay_ms(150);
        leds_off();
        _delay_ms(250);
        ledscheme = (slave_id & 0xF0) >> 4;
        if( ledscheme & 0x01 ) led_yellow_on();
        if( ledscheme & 0x02 ) led_green_on();
        if( ledscheme & 0x04 ) led_orange_on();
        if( ledscheme & 0x08 ) led_red_on();
        _delay_ms(2000);
        leds_off();
        _delay_ms(150);
        led_red_on();
        _delay_ms(150);
        led_orange_on();
        _delay_ms(150);
        led_green_on();
        _delay_ms(150);
        led_yellow_on();
        _delay_ms(150);
        leds_off();
        _delay_ms(250);
        ledscheme = (slave_id & 0x0F);
        if( ledscheme & 0x01 ) led_yellow_on();
        if( ledscheme & 0x02 ) led_green_on();
        if( ledscheme & 0x04 ) led_orange_on();
        if( ledscheme & 0x08 ) led_red_on();
        _delay_ms(2000);
        leds_off();
        _delay_ms(200);

        armed = debounce( &KEY_PIN, KEY );

        if ( armed ) {
            led_red_on();
        }
        else {
            led_red_off();
        }

        flags.b.clear_list = 1;
        tx_field[0]        = PARAMETERS;
        tx_field[1]        = unique_id;
        tx_field[2]        = slave_id;
        tx_field[3]        = adc_read( 5 );
        tx_field[4]        = armed;
        tx_field[5]        = temperature;
    }

    flags.b.transmit = 1;

    // Enable Interrupts
    sei();

    // Main loop

    /*
     * Within the main loop the status of all the flags is continuously monitored and upon detection of a set flag
     * the corresponding routine is executed. Before the execution of the routine the status register is written to a local
     * variable, interrupts are disabled and the flag gets cleared, after the routine interrupts are re-enabled
     *
     */
    while ( 1 ) {
        // -------------------------------------------------------------------------------------------------------

        // Control key switch (key_flag gets set via pin-change-interrupt)
        if ( key_flag ) {
            temp_sreg = SREG;
            cli();
            key_flag = 0;

            // Box armed: armed = 1, Box not armed: armed = 0
            armed = debounce( &KEY_PIN, KEY );

            if ( armed ) {
                led_red_on();
            }
            else {
                led_red_off();
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // UART-Routine
        flags.b.uart_active = ( ( UCSR0A & ( 1 << RXC0 ) ) && 1 );

        if ( flags.b.uart_active ) {
            temp_sreg = SREG;
            cli();
            flags.b.uart_active = 0;

            led_yellow_on();

            // Receive first char
            uart_field[0] = uart_getc();

            // React according to first char (ignition command or not?)
            // Ignition command is always 4 chars long
            switch ( uart_field[0] ) {
                case 0xFF: {
                    uart_field[1] = uart_getc();
                    uart_field[2] = uart_getc();
                    uart_field[3] = uart_getc();
                    uart_field[4] = '\0';
                    break;
                }

                // Any other command is received as long as it doesn't start with enter or backspace
                case 8:
                case 10:
                case 13:
                case 127: {
                    uart_field[0] = '\0';
                    break;
                }

                default: {
                    #if !CASE_SENSITIVE
                        uart_field[0] = uart_lower_case( uart_field[0] );
                    #endif
                    uart_putc( uart_field[0] ); // Show first char so everything looks as it should

                    if ( !uart_gets( uart_field + 1 ) ) {
                        uart_puts_P( PSTR( "\033[1D" ) );
                        uart_puts_P( PSTR( " " ) );
                        uart_puts_P( PSTR( "\033[1D" ) );
                        uart_field[0] = '\0';
                    }

                    break;
                }
            }

            // Evaluate inputs
            // "conf" starts ID configuration
            if ( uart_strings_equal( uart_field, "conf" ) ) {
                flags.b.uart_config = 1;
                flags.b.transmit    = 0;
            }

            // "remote" starts remote ID configuration
            if ( uart_strings_equal( uart_field, "remote" ) ) {
                flags.b.remote   = 1;
                flags.b.transmit = 0;
            }

            // "clearlist" empties list of boxes
            if ( uart_strings_equal( uart_field, "clearlist" ) ) {
                flags.b.clear_list = 1;
            }

            // "send" allows to manually send a command
            if (  uart_strings_equal( uart_field, "send" ) || uart_strings_equal( uart_field, "fire" )
               || uart_strings_equal( uart_field, "ident" ) || uart_strings_equal( uart_field, "temp" ) ) {
                flags.b.send     = 1;
                flags.b.transmit = 0;
            }

            // "list" gives a overview over connected boxes
            if ( uart_strings_equal( uart_field, "list" ) ) {
                flags.b.list     = 1;
                flags.b.transmit = 0;
            }

            // "orders" shows last transmitted and received command on the LCD
            if ( uart_strings_equal( uart_field, "orders" ) && TRANSMITTER ) {
                flags.b.rx_post    = 1;
                flags.b.tx_post    = 1;
                flags.b.show_only  = 1;
                flags.b.lcd_update = 1;
            }

            // "arm" arms transmitter
            if ( uart_strings_equal( uart_field, "arm" ) && TRANSMITTER ) {
                armed = 1;
                led_red_on();
            }

            // "disarm" disarms transmitter
            if ( uart_strings_equal( uart_field, "disarm" ) && TRANSMITTER ) {
                armed = 0;
                led_red_off();
            }

            // "cls" clears terminal screen
            if ( uart_strings_equal( uart_field, "cls" ) ) {
                terminal_reset();
            }

            // "kill" resets the controller
            if ( uart_strings_equal( uart_field, "kill" ) ) {
                flags.b.reset_device = 1;
            }

            // "hardware" or "hw" outputs for uC- and rfm-type
            if ( uart_strings_equal( uart_field, "hw" ) || uart_strings_equal( uart_field, "hardware" ) ) {
                flags.b.hw = 1;
            }

            // "rfm" gives access to radio module
            if ( uart_strings_equal( uart_field, "rfm" ) ) {
                scheme = rfmtalk();

                if ( scheme != 0xFFFF ) {
                    #if ( RFM == 69 )
                        rfm_pwr = 0;

                        if ( ( scheme & 0xFFE0 ) == 0x9180 ) {
                            rfm_pwr = ( scheme & 0x001F );
                        }

                        scheme = rfm_cmd( scheme, ( scheme & 32768 ) && 1 );
                    #else
                        scheme = rfm_cmd( scheme );
                    #endif
                    uart_puts_P( PSTR( " --> : 0x" ) );
                    uart_shownum( scheme, 'h' );

                    #if ( RFM == 69 )

                        if ( rfm_pwr ) {
                            uart_puts_P( PSTR( "\r\nSendeleistung dauerhaft speichern (j/n)? " ) );
                            inp = 0;

                            while ( !( ( inp == 'j' ) || ( inp == 'n' ) ) ) inp = uart_getc() | 0x20;

                            uart_putc( inp );
                            uart_puts_P( PSTR( "\r\n" ) );

                            if ( inp == 'j' ) {
                                eewrite( rfm_pwr, RFM_PWR_ADDRESS );
                                eewrite( crc8( 0x11, rfm_pwr ), ( RFM_PWR_ADDRESS + 1 ) );
                                uart_puts_P( PSTR( "Speichern erfolgreich!\r\n" ) );
                            }
                        }

                    #endif
                }

                uart_puts( "\n\n\r" );
            }

            // "aeskey" displays the current key and allows to set a new one
            if ( uart_strings_equal( uart_field, "aeskey" ) ) {
                changes = aesconf();

                if ( changes ) {
                    flags.b.reset_device = 1;
                }

                changes = 0;
            }

            // If valid ignition command was received
            if ( fire_command_uart_valid( uart_field ) ) {
                // Transmit to everybody
                tx_field[0]          = FIRE;
                tx_field[1]          = uart_field[1];
                tx_field[2]          = uart_field[2];
                flags.b.transmit     = 1;
                transmission_allowed = 1;

                // Check if ignition was triggered on device that received the serial command
                if ( ( slave_id == uart_field[1] ) && !TRANSMITTER ) {
                    tmp = uart_field[2] - 1;

                    if ( !flags.b.fire ) {
                        rx_field[2]  = uart_field[2];
                        flags.b.fire = 1;
                        loopcount    = 1;
                    }
                }
            }

            led_yellow_off();

            if ( uart_field[0] && ( uart_field[0] != 0xFF ) ) {
                uart_puts_P( PSTR( "\n\r" ) );
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Hardware
        if ( flags.b.hw ) {
            temp_sreg = SREG;
            cli();
            flags.b.hw = 0;

            uart_puts_P( PSTR( "\n\r" ) );
            uart_puts( ig_or_notrans ? "Zündbox v1/v2" : "Transmitter" );
            uart_puts_P( PSTR( "\n\r" ) );
            uart_puts_P( PSTR( STRINGIZE_VALUE_OF( MCU ) ) );
            uart_puts_P( PSTR( "\n\rRFM" ) );
            uart_shownum( RFM, 'd' );
            #if defined COMPILEDATE && defined COMPILETIME
                uart_puts_P( PSTR( "\n\r" ) );
                uart_puts_P( PSTR( "Datecode " ) );
                uart_puts_P( PSTR( STRINGIZE_VALUE_OF( COMPILEDATE ) ) );
                uart_puts_P( PSTR( STRINGIZE_VALUE_OF( COMPILETIME ) ) );
            #endif
            uart_puts_P( PSTR( "\n\n\r" ) );

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Slave- and Unique-ID settings
        if ( flags.b.uart_config ) {
            temp_sreg = SREG;
            cli();
            flags.b.uart_config = 0;

            changes = configprog( ig_or_notrans );

            if ( changes ) {
                flags.b.reset_device = 1;
            }

            changes = 0;
            SREG    = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Remote Slave- and Unique-ID settings
        if ( flags.b.remote ) {
            temp_sreg = SREG;
            cli();
            flags.b.remote = 0;

            changes = 0;
            changes = remote_config( tx_field );

            if ( changes ) {
                // If the numbers are those of the connected device
                if ( ( tx_field[1] == unique_id ) && ( tx_field[2] == slave_id ) ) {
                    uart_puts_P( PSTR( "\n\rIDs werden lokal angepasst!\n\r" ) );
                    addresses_save( tx_field[3], tx_field[4] );
                    flags.b.reset_device = 1;
                }
                // If not...
                else {
                    uart_puts_P( PSTR( "\n\rID-Konfigurationsbefehl wird gesendet!\n\r" ) );
                    flags.b.transmit = 1;
                }

                uart_puts_P( PSTR( "\n\n\r" ) );
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Manual transmission
        if ( flags.b.send ) {
            temp_sreg = SREG;
            cli();

            flags.b.send = 0;

            nr = 0, tmp = 0, inp = 0;

            switch ( uart_field[0] ) {
                case FIRE:
                case IDENT:
                case TEMPERATURE: {
                    inp = uart_field[0];
                    break;
                }

                default: {
                    uart_puts_P( PSTR( "\n\rModus(f/i/t): " ) );

                    while ( !inp ) inp = uart_getc() | 0x20;

                    uart_putc( inp );
                    uart_puts_P( PSTR( "\r\n" ) );
                    break;
                }
            }

            tx_field[0] = inp;

            if ( ( tx_field[0] == FIRE ) || ( tx_field[0] == IDENT ) || ( tx_field[0] == TEMPERATURE ) ) {
                // Assume, that a transmission shall take place
                tmp = 1;

                // Handle slave-id and channel input
                switch ( tx_field[0] ) {
                    case FIRE: {
                        for ( uint8_t round = 1; round < 3; round++ ) {                                  // Loop twice
                            nr = 0;
                            uart_puts_P( round < 2 ? PSTR( "Slave-ID:\t" ) : PSTR( "\n\rKanal:  \t" ) ); // First for slave-id, then for channel

                            for ( i = 0; i < 2; i++ ) {                                 // Get the user to assign the numbers with 2 digits
                                inp = 0;

                                while ( !inp ) inp = uart_getc();

                                uart_putc( inp );
                                nr *= 10;
                                nr += ( inp - '0' );
                            }

                            uart_puts_P( PSTR( " = " ) );

                            if ( ( nr > 0 ) && ( nr < ( ( round < 2 ) ? (MAX_ID+1) : 17 ) ) ) { // Slave-ID has to be 1-MAX_ID, Channel 1-16
                                uart_shownum( nr, 'd' );
                                tx_field[round] = nr;
                            }
                            else {                                                      // Otherwise the input's invalid
                                uart_puts_P( PSTR( "Ungültige Eingabe" ) );
                                tmp   = 0;                                              // Sending gets disallowed
                                round = 3;
                            }
                        }

                        break;
                    }

                    case IDENT: {
                        tx_field[0] = IDENT;
                        tx_field[1] = 'd';
                        tx_field[2] = '0';
                        break;
                    }

                    case TEMPERATURE: {
                        temperature = tempmeas( tempsenstype );

                        uart_puts_P( PSTR( "Temperatur: " ) );

                        if ( temperature == -128 ) {
                            uart_puts_P( PSTR( "n.a." ) );
                        }
                        else {
                            fixedspace( temperature, 'd', 4 );
                            uart_puts_P( PSTR( "°C" ) );
                        }

                        // Request other devices to refresh temperature as well
                        tx_field[0] = TEMPERATURE;
                        tx_field[1] = 'e';
                        tx_field[2] = 'm';
                        tx_field[3] = 'p';
                        break;
                    }

                    default: {
                        break;
                    }
                }
            }

            uart_puts_P( PSTR( "\n\n\r" ) );

            // Take action after proper command
            if ( tmp ) {
                flags.b.transmit     = 1;
                transmission_allowed = 1;

                if ( ( tx_field[0] == FIRE ) && ( slave_id == tx_field[1] ) ) {
                    rx_field[2]  = tx_field[2];
                    flags.b.fire = 1;
                }
            }
            else {
                flags.b.transmit = 0;
            }

            while ( UCSR0A & ( 1 << RXC0 ) ) inp = UDR0;

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // List network devices
        if ( flags.b.list ) {
            temp_sreg = SREG;
            cli();

            flags.b.list = 0;

            list_complete( slaves, iderrors );
            evaluate_boxes( slaves, quantity );
            list_array( quantity );

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Software-Reset via Watchdog
        if ( flags.b.reset_device ) {
            cli();

            if ( TRANSMITTER ) {
                lcd_clear();
                lcd_puts( "Resetting device!" );
            }
            else {
                sr_disable();
            }

            wdt_enable( 6 );
            terminal_reset();

            while ( 1 );
        }

        // -------------------------------------------------------------------------------------------------------

        // Clear list of ignition devices
        if ( flags.b.clear_list || ( flags.b.transmit && ( tx_field[0] == IDENT ) ) ) {
            temp_sreg = SREG;
            cli();

            flags.b.clear_list = 0;

            iderrors = 0;

            for ( i = 0; i < MAX_ID; i++ ) {
                quantity[i]               = 0;
                slaves[i].slave_id        = 0;
                slaves[i].battery_voltage = 0;
                slaves[i].sharpness       = 0;
                slaves[i].temperature     = -128;
                slaves[i].rssi            = -128;
            }

            // Ignition devices have to write themselves in the list
            if ( !TRANSMITTER ) {
                quantity[slave_id - 1]                = 1;
                slaves[unique_id - 1].slave_id        = slave_id;
                slaves[unique_id - 1].battery_voltage = adc_read( 5 );
                slaves[unique_id - 1].sharpness       = ( armed ? 'j' : 'n' );
                slaves[unique_id - 1].temperature     = temperature;
                slaves[unique_id - 1].rssi            = 0;
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Transmit
        // Check if device has waited long enough (according to unique-id) to be allowed to transmit
        if ( !transmission_allowed && ( ( ( timer1_flags & TIMER_TRANSMITCOUNTER_FLAG ) && ( transmit_flag > ( unique_id * 10U + 10U ) ) ) || TRANSMITTER ) ) {
            transmission_allowed = 1;
        }

        // Transmission process
        if ( flags.b.transmit && transmission_allowed ) {
            temp_sreg = SREG;
            cli();

            flags.b.transmit = 0;

            switch ( tx_field[0] ) {
                setTxCase( FIRE );
                setTxCase( CHANGE );
                setTxCase( IDENT );
                setTxCase( TEMPERATURE );
                setTxCase( PARAMETERS );
                setTxCase( MEASURE );
                setTxCase( IMPEDANCES );

                default: {
                    loopcount = 0;
                    tmp       = 0;
                    break;
                }
            }

            tx_field[tmp]     = loopcount;
            tx_field[tmp + 1] = '\0';
            tx_length         = tmp + 1;

            if ( ( tx_field[0] != FIRE ) || armed ) { // Only send 'FIRE' if sending device is armed
                for ( uint8_t i = loopcount; i; i-- ) {
                    led_green_on();

                    rfm_tx_error = rfm_transmit( tx_field, tx_length ); // Transmit message
                    tx_field[tmp]--;

                    led_green_off();
                }

                flags.b.lcd_update = 1;
                flags.b.tx_post    = 1;
            }

            timer1_flags        &= ~TIMER_TRANSMITCOUNTER_FLAG;
            transmit_flag        = 0;
            transmission_allowed = 0;

            rfm_rxon();

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Fire
        if ( flags.b.fire ) {
            temp_sreg = SREG;
            cli();
            flags.b.fire = 0;

            if ( armed && ( rx_field[2] > 0 ) && ( rx_field[2] < 17 ) ) { // If channel number is valid
                tmp = rx_field[2];                                        // Save channel number to variable

                flags.b.is_fire_active = 1;                               // Signalize that we're currently firing

                // Turn all leds on
                leds_on();

                scheme = 0;                                               // Set mask-variable to zero

                for ( uint8_t i = 16; i; i-- ) {
                    scheme <<= 1;    // Left-shift mask-variable

                    if ( i == tmp ) {
                        scheme |= 1; // Set LSB if loop variable equals channel number

                    }
                }

                scheme |= active_channels;

                MOSSWITCHPORT |= ( 1 << MOSSWITCH );
                sr_shiftout( scheme ); // Write pattern to shift-register
                active_channels = scheme;
            }

            // Turn on receiver
            rfm_rxon();

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        if ( flags.b.is_fire_active && channel_monitor ) { // If we're currently firing and monitoring is due
            temp_sreg = SREG;
            cli();

            channel_monitor = 0;                           // Clear the monitoring flag
            anti_scheme     = 0;                           // Reset the delete scheme

            controlvar = 1;
            for ( uint8_t i = 0; i < 16; i++ ) {
                if ( active_channels & controlvar ) { // If a given channel is currently active
                    channel_timeout[i]++;             // Increment the timeout-value for that channel

                    if ( channel_timeout[i] >= IGNITION_TIME ) { // If the channel was active for at least the ignition time
                        anti_scheme          |= controlvar;      // Set delete-bit for this channel
                        channel_timeout[i]    = 0;               // Reset channel-timeout value
                        flags.b.finish_firing = 1;               // Leave a note that a change in the list of active channels is due
                    }
                }

                controlvar <<= 1; // Left-shift mask-variable
            }
            anti_scheme ^= active_channels; // Set channels to zero, which shell be deleted AND are active, others remain active.
            SREG         = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Stop firing if it's due
        if ( flags.b.finish_firing ) {      // If we have the order to stop firing on at least one channel
            temp_sreg = SREG;
            cli();
            flags.b.finish_firing = 0;      // Mark the order as done

            // Lock respective MOSFETs
            sr_shiftout( anti_scheme );     // Perform the necessary shift-register changes

            active_channels = anti_scheme;  // Re-write the list of currently active channels

            if ( !anti_scheme ) {                             // If no more channels are active at the moment
                MOSSWITCHPORT         &= ~( 1 << MOSSWITCH ); // Block the P-FET-channel
                flags.b.is_fire_active = 0;                   // Signalize that firing is finished for now

                // Turn all LEDs off and the red one on again
                leds_off();
                led_red_on();
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Check receive flag
        temp_sreg = SREG;
        cli();
        flags.b.receive = rfm_receiving();
        SREG            = temp_sreg;

        // Receive
        if ( flags.b.receive ) {
            temp_sreg = SREG;
            cli();

            flags.b.receive = 0;

            led_orange_on();
            #ifdef RFM69_H_
                rssi = rfm_get_rssi_dbm();                      // Measure signal strength (RFM69 only)
            #endif
            rfm_rx_error = rfm_receive( rx_field, &rx_length ); // Get Message
            led_orange_off();

            if ( rfm_rx_error ) {
                rx_field[0] = ERROR;
            }
            else {
                switch ( rx_field[0] ) { // Act according to type of message received
                    // Received ignition command (only relevant for ignition devices)
                    case FIRE: {
                        // Wait for all repetitions to be over
                        waitRx( FIRE );

                        if ( armed && ( rx_field[1] == slave_id ) && !TRANSMITTER ) {
                            tmp = rx_field[2] - 1;

                            if ( !flags.b.fire ) {
                                flags.b.fire = 1;
                            }
                        }

                        break;
                    }

                    // Received temperature-measurement-trigger
                    case TEMPERATURE: {
                        // Wait for all repetitions to be over
                        waitRx( TEMPERATURE );

                        temperature = tempmeas( tempsenstype );
                        break;
                    }

                    // Received identification-demand
                    case IDENT: {
                        // Wait for all repetitions to be over
                        waitRx( IDENT );

                        tx_field[0] = PARAMETERS;
                        tx_field[1] = unique_id;
                        tx_field[2] = slave_id;
                        tx_field[3] = ( TRANSMITTER ? 50 : adc_read( 5 ) );
                        tx_field[4] = armed;
                        tx_field[5] = temperature;

                        transmission_allowed = 0;

                        timer1_reset();
                        timer1_flags |= TIMER_TRANSMITCOUNTER_FLAG;
                        transmit_flag = 0;

                        flags.b.transmit   = 1;
                        flags.b.clear_list = 1;

                        break;
                    }

                    // Received Parameters
                    case PARAMETERS: {
                        // Wait for all repetitions to be over
                        waitRx( PARAMETERS );

                        // Increment ID error, if ID-error (='E') or 0 or unique-id of this device
                        // or already used unique-id was received as unique-id
                        if (   ( rx_field[1] == 'E' ) || ( !rx_field[1] ) || ( rx_field[1] == unique_id )
                           || ( rx_field[1] && slaves[(uint8_t)( rx_field[1] - 1 )].slave_id ) ) {
                            iderrors++;
                        }
                        else {
                            tmp                         = rx_field[1] - 1; // Index = unique_id-1 (zero-based indexing)
                            slaves[tmp].slave_id        = rx_field[2];
                            slaves[tmp].battery_voltage = rx_field[3];
                            slaves[tmp].sharpness       = ( rx_field[4] ? 'j' : 'n' );
                            slaves[tmp].temperature     = rx_field[5];
                            slaves[tmp].rssi            = rssi;
                        }

                        break;
                    }

                    // Received change command
                    case CHANGE: {
                        // Wait for all repetitions to be over
                        waitRx( CHANGE );

                        if ( !armed && ( unique_id == rx_field[1] ) && ( slave_id == rx_field[2] ) ) {
                            rem_uid = rx_field[3];
                            rem_sid = rx_field[4];

                            // Change IDs if they are in the valid range (1-MAX_ID) and at least one of the two IDs is
                            // a different value than before
                            if (   ( ( rem_uid > 0 ) && ( rem_uid < (MAX_ID+1) ) ) && ( ( rem_sid > 0 ) && ( rem_sid < (MAX_ID+1) ) )
                               && ( ( rem_uid != unique_id ) || ( rem_sid != slave_id ) ) ) {
                                addresses_save( rem_uid, rem_sid );
                                flags.b.reset_device = 1;
                            }
                        }

                        break;
                    }

                    case MEASURE: {
                        // Wait for all repetitions to be over
                        waitRx( MEASURE );

                        // Send empty impedance list because we cannot measure with version 1 and 2
                        if ( unique_id == rx_field[1] ) {
                            tx_field[0] = IMPEDANCES;
                            tx_field[1] = unique_id;

                            for ( uint8_t i = 0; i < 16; i++ ) {
                                tx_field[2 + i] = 0x00;
                            }

                            flags.b.transmit     = 1;
                            transmission_allowed = 0;
                            timer1_reset();
                            timer1_flags |= TIMER_TRANSMITCOUNTER_FLAG;
                            transmit_flag = 10 * unique_id; // Preload for 100ms delay
                        }

                        break;
                    }

                    // Default action (do nothing)
                    default: {
                        break;
                    }
                }

                flags.b.lcd_update = 1;
                flags.b.rx_post    = 1;
            }

            rfm_rxon();
            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Clear LCD in case of timeouts
        if ( TRANSMITTER && !flags.b.lcd_update && (   ( clear_lcd_tx_flag > DEL_THRES ) || ( clear_lcd_rx_flag > DEL_THRES )
                                                   || ( hist_del_flag > ( 3 * DEL_THRES ) ) ) ) {
            temp_sreg = SREG; // Speichere Statusregister
            cli();

            if ( clear_lcd_tx_flag > DEL_THRES ) {
                clear_lcd_tx_flag = 0;
                lcd_cursorset( 1, 1 );

                for ( i = 0; i < 20; i++ ) {
                    lcd_puts( " " );
                }
            }

            if ( clear_lcd_rx_flag > DEL_THRES ) {
                clear_lcd_rx_flag = 0;
                lcd_cursorset( 2, 1 );

                for ( i = 0; i < 20; i++ ) {
                    lcd_puts( " " );
                }
            }

            if ( hist_del_flag > ( DEL_THRES * 3 ) ) {
                hist_del_flag = 0;
                lcd_cursorset( 3, 1 );

                for ( i = 0; i < 20; i++ ) {
                    lcd_puts( "  " );
                }

                anzzeile  = 3;
                anzspalte = 1;
            }

            SREG = temp_sreg;
        }

        // -------------------------------------------------------------------------------------------------------

        // Refresh LCD
        if ( TRANSMITTER && flags.b.lcd_update ) {
            temp_sreg = SREG;
            cli();

            flags.b.lcd_update = 0;

            // TRANSMITTER (1. Line + 3./4. Line)
            if ( flags.b.tx_post ) {
                lcd_cursorset( 1, 1 );
                lcd_puts( "Tx:" );

                if ( rfm_tx_error ) {
                    rfm_tx_error = 0;
                    lcd_puts( "ERROR            " );
                }
                else {
                    switch ( tx_field[0] ) {
                        case FIRE: {
                            if ( tx_field[1] && ( tx_field[1] < (MAX_ID+1) ) && tx_field[2] && ( tx_field[2] < 17 ) ) {
                                lcd_send( 0, 1 );
                                lcd_puts( " S" );
                                lcd_arrize( tx_field[1], lcd_array, 2, 0 );
                                lcd_puts( lcd_array );
                                lcd_puts( " CH" );
                                lcd_arrize( tx_field[2], lcd_array, 2, 0 );
                                lcd_puts( lcd_array );
                                lcd_puts( "       " );

                                if ( !flags.b.show_only ) {
                                    lcd_cursorset( lastzeile, lastspalte );
                                    lcd_puts( " " );
                                    lcd_cursorset( anzzeile, anzspalte );
                                    lcd_puts( "x" );
                                    lcd_arrize( tx_field[1], lcd_array, 2, 0 );
                                    lcd_puts( lcd_array );
                                    lcd_send( 0, 1 );
                                    lcd_arrize( tx_field[2], lcd_array, 2, 0 );
                                    lcd_puts( lcd_array );

                                    cursor_x_shift( &lastzeile, &lastspalte, &anzzeile, &anzspalte );
                                    hist_del_flag = 1;
                                }
                            }

                            flags.b.show_only = 0;
                            break;
                        }

                        case IDENT: {
                            lcd_puts( "Identify         " );

                            if ( !flags.b.show_only ) {
                                lcd_cursorset( lastzeile, lastspalte );
                                lcd_puts( " " );
                                lcd_cursorset( anzzeile, anzspalte );
                                lcd_puts( "xIDENT" );

                                cursor_x_shift( &lastzeile, &lastspalte, &anzzeile, &anzspalte );
                                hist_del_flag = 1;
                            }

                            flags.b.show_only = 0;
                            break;
                        }

                        case PARAMETERS: {
                            lcd_puts( "U" );                                     // Unique-ID
                            lcd_arrize( tx_field[1], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( "S" );                                     // Salve-ID
                            lcd_arrize( tx_field[2], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " " );

                            if ( tx_field[3] < 100 ) {
                                lcd_puts( " " );
                            }

                            lcd_arrize( ( tx_field[3] / 10 ), lcd_array, 1, 0 ); // Battery voltage
                            lcd_puts( lcd_array );
                            lcd_puts( "." );
                            lcd_arrize( ( tx_field[3] % 10 ), lcd_array, 1, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " " );
                            lcd_send( tx_field[4] ? 'j' : 'n', 1 );              // Armed?
                            lcd_puts( "    " );
                            break;
                        }

                        case TEMPERATURE: {
                            lcd_puts( "Temperature      " );

                            if ( !flags.b.show_only ) {
                                lcd_cursorset( lastzeile, lastspalte );
                                lcd_puts( " " );
                                lcd_cursorset( anzzeile, anzspalte );
                                lcd_puts( "xTEMP " );

                                cursor_x_shift( &lastzeile, &lastspalte, &anzzeile, &anzspalte );
                                hist_del_flag = 1;
                            }

                            flags.b.show_only = 0;

                            break;
                        }

                        case CHANGE: {
                            lcd_puts( "U" );                                     // Old Unique-ID
                            lcd_arrize( tx_field[1], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " S" );                                    // Old Slave-ID
                            lcd_arrize( tx_field[2], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( "->U" );                                   // New Unique-ID
                            lcd_arrize( tx_field[3], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " S" );                                    // New Slave-ID
                            lcd_arrize( tx_field[4], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " " );
                            break;
                        }

                        default: {
                            break;
                        }
                    }
                }

                flags.b.tx_post   = 0;
                clear_lcd_tx_flag = 1;
            }

            // RECEIVER (2. Line)
            if ( flags.b.rx_post ) {
                lcd_cursorset( 2, 1 );
                lcd_puts( "Rx:" );

                switch ( rx_field[0] ) {
                    case FIRE: {
                        if ( rx_field[1] && ( rx_field[1] < (MAX_ID+1) ) && rx_field[2] && ( rx_field[2] < 17 ) ) {
                            lcd_send( 0, 1 );
                            lcd_puts( " S" );
                            lcd_arrize( rx_field[1], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " CH" );
                            lcd_arrize( rx_field[2], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( "    -" );

                            if ( !rssi ) {
                                lcd_puts( "--" );
                            }
                            else {
                                lcd_arrize( ( ( rssi > 99 ) ? 99 : rssi ), lcd_array, 2, 0 );
                                lcd_puts( lcd_array );
                            }
                        }

                        break;
                    }

                    case IDENT: {
                        lcd_puts( "Identify      -" );

                        if ( !rssi ) {
                            lcd_puts( "--" );
                        }
                        else {
                            lcd_arrize( ( ( rssi > 99 ) ? 99 : rssi ), lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                        }

                        break;
                    }

                    case PARAMETERS: {
                        lcd_puts( "U" );
                        lcd_arrize( rx_field[1], lcd_array, 2, 0 );
                        lcd_puts( lcd_array );
                        lcd_puts( "S" );
                        lcd_arrize( rx_field[2], lcd_array, 2, 0 );
                        lcd_puts( lcd_array );
                        lcd_puts( " " );

                        if ( rx_field[3] < 100 ) {
                            lcd_puts( " " );
                        }

                        lcd_arrize( rx_field[3] / 10, lcd_array, 1, 0 );
                        lcd_puts( lcd_array );
                        lcd_puts( "." );
                        lcd_arrize( rx_field[3] % 10, lcd_array, 1, 0 );
                        lcd_puts( lcd_array );
                        lcd_puts( " " );
                        lcd_send( rx_field[4] ? 'j' : 'n', 1 );
                        lcd_puts( " -" );

                        if ( !rssi ) {
                            lcd_puts( "--" );
                        }
                        else {
                            lcd_arrize( ( ( rssi > 99 ) ? 99 : rssi ), lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                        }

                        break;
                    }

                    case CHANGE: {
                        if (  rx_field[1] && rx_field[2] && rx_field[3] && rx_field[4] && ( rx_field[1] < (MAX_ID+1) )
                           && ( rx_field[2] < (MAX_ID+1) ) && ( rx_field[3] < (MAX_ID+1) ) && ( rx_field[1] < (MAX_ID+1) ) ) {
                            lcd_puts( "U" );   // Old Unique-ID
                            lcd_arrize( rx_field[1], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " S" );  // Old Slave-ID
                            lcd_arrize( rx_field[2], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( "->U" ); // New Unique-ID
                            lcd_arrize( rx_field[3], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " S" );  // New Slave-ID
                            lcd_arrize( rx_field[4], lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                            lcd_puts( " " );
                        }

                        break;
                    }

                    case TEMPERATURE: {
                        lcd_puts( "Temperature   -" );

                        if ( !rssi ) {
                            lcd_puts( "--" );
                        }
                        else {
                            lcd_arrize( ( ( rssi > 99 ) ? 99 : rssi ), lcd_array, 2, 0 );
                            lcd_puts( lcd_array );
                        }

                        break;
                    }

                    default: {
                        break;
                    }
                }

                flags.b.rx_post   = 0;
                clear_lcd_rx_flag = 1;
            }

            SREG = temp_sreg;
        }
    }

    // -------------------------------------------------------------------------------------------------------

    return 0;
}

// Interrupt vectors
ISR( TIMER1_COMPA_vect ) { // Occurs every 10ms if active
    if ( timer1_flags & TIMER_TRANSMITCOUNTER_FLAG ) {
        transmit_flag++;
    }

    // -------------------------------------------------------------------------------------------------------

    if ( active_channels ) {
        channel_monitor = 1; // Set a reminder to monitor the channels
    }
}

ISR( TIMER0_OVF_vect ) {
    if ( clear_lcd_tx_flag ) {
        clear_lcd_tx_flag++;
    }

    if ( clear_lcd_rx_flag ) {
        clear_lcd_rx_flag++;
    }

    if ( hist_del_flag ) {
        hist_del_flag++;
    }

    if ( clear_lcd_tx_flag > ( DEL_THRES + 2 ) ) {
        clear_lcd_tx_flag = 0;
    }

    if ( clear_lcd_rx_flag > ( DEL_THRES + 2 ) ) {
        clear_lcd_rx_flag = 0;
    }

    if ( hist_del_flag > 10000 ) {
        hist_del_flag = 0;
    }
}

ISR( KEYINT ) {
    key_flag = 1;
}
