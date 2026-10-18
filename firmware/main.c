/**
 * @file main.c
 * @brief Pico.Lab Firmware
 * 2026 Maximilian Kern
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

// Board API & USB Stack
#include "bsp/board_api.h"
#include "tusb.h"

// Pico SDK Hardware Drivers
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/watchdog.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "pico/multicore.h"

// Network Services & lwIP
#include "dhserver.h"
#include "dnserver.h"
#include "httpd.h"
#include "lwip/apps/httpd.h"
#include "lwip/ethip6.h"
#include "lwip/init.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"

#include "board_config.h"

#if PICOLAB_HAS_NEOPIXEL
#include "pico/status_led.h"
#endif

#ifdef INCLUDE_IPERF
  #include "lwip/apps/lwiperf.h"
#endif

#include "logic_analyzer.pio.h"

// Convenience macro to construct netif IPv4 addresses cleanly
#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

#ifndef PICO_DEFAULT_LED_PIN
    #define PICO_DEFAULT_LED_PIN 20
#endif

#define NUM_MANAGED_PINS PICOLAB_NUM_DIGITAL_PINS
#define NUM_ANALOG_CHANNELS PICOLAB_NUM_ANALOG_CHANNELS
#define MANAGED_PINS PICOLAB_MANAGED_PINS

// Connection state blinking frequencies (ms)
enum {
    BLINK_NOT_MOUNTED = 250,
    BLINK_MOUNTED = 1000,
    BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;
static struct netif netif_data;

// Transmit buffer queue for offloading network frames to TinyUSB
#define TX_PBUF_QUEUE_LEN 64
static struct pbuf* tx_pbuf_queue[TX_PBUF_QUEUE_LEN];
static int tx_pbuf_head = 0;
static int tx_pbuf_tail = 0;

// Static IP and DHCP lease allocations for RNDIS/ECM USB ethernet link
uint8_t tud_network_mac_address[6] = {0x02, 0x02, 0x84, 0x6A, 0x96, 0x00};
static const ip4_addr_t ipaddr  = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

// Pool of IP addresses the server is allowed to hand out
static dhcp_entry_t entries[] = {
    {{0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 4), 24 * 60 * 60},
};

// Configuration for the USB network DHCP server
static const dhcp_config_t dhcp_config = {
    .router = INIT_IP4(0, 0, 0, 0),     // No default gateway (local network only)
    .port = 67,                         // Standard DHCP server port
    .dns = INIT_IP4(192, 168, 7, 1),    // Local DNS server IP
    "usb",                              // Network interface name
    TU_ARRAY_SIZE(entries),             // Number of available IP addresses to hand out
    entries                             // Array containing the actual IP addresses
};

// Global system information variables for keeping track of Flash, RAM and stuff.
char global_serial_str[17] = {0};
uint32_t global_flash_mb = 0;
extern char __flash_binary_start;       // The start address of the program in flash memory
extern char __flash_binary_end;         // The end address of the program in flash memory
extern char __StackTop;                 // The top of the SRAM (used to calculate total RAM)
extern char __bss_end__;                // The end of static variables (used to calculate used RAM)

// Calculates total hardware RAM based on linker stack boundaries
uint32_t get_total_ram(void) {
    return (uint32_t)&__StackTop - SRAM_BASE;
}

// Calculates statically allocated RAM (BSS/DATA) + any dynamic heap
uint32_t get_used_ram(void) {
    uint32_t static_ram = (uint32_t)&__bss_end__ - SRAM_BASE;
    struct mallinfo m = mallinfo();
    return static_ram + m.uordblks;
}

// Deferred system reboot flags handled during main core loop
volatile bool reboot_to_bootsel_pending = false;
volatile bool send_sysinfo_pending = false;
volatile bool reboot_normal_pending = false;

// UART Rx and Tx ring buffers for serial bridging over HTTP/SSE
#define UART_RX_BUF_SIZE 256
static char uart_rx_buffer[UART_RX_BUF_SIZE];
static int uart_rx_len = 0;
static bool has_new_uart_data = false;

#define UART_TX_BUF_SIZE 2048
static uint8_t uart_tx_buffer[UART_TX_BUF_SIZE];
static uint16_t uart_tx_head = 0;
static uint16_t uart_tx_tail = 0;

#define CLI_RX_BUF_SIZE 256
static char cli_rx_buffer[CLI_RX_BUF_SIZE];
static int cli_rx_len = 0;

// Forward Declarations for CLI Tasks
static void cli_write_all(const char *data, size_t len);
static bool parse_uint_auto(const char *text, unsigned int *value);

// Server-Sent Events (SSE) state
struct tcp_pcb *sse_pcb = NULL;                         // Active TCP connection for the SSE stream
static bool sse_headers_sent = false;                   // Tracks if initial HTTP response headers were sent
static volatile bool browser_connection_active = false; // True if a client dashboard is currently connected
// Tracks the active HTTP POST connection (used for UART TX routing)
static void *current_post_connection = NULL;
// Buffer and flag for queuing I2C/SPI transaction results to send via SSE
static bool bus_event_pending = false;                  
static char bus_event_payload[512] = {0};

// Formats a JSON string for I2C/SPI bus events and flags it for transmission.
static void queue_bus_event_json(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(bus_event_payload, sizeof(bus_event_payload), format, args);
    va_end(args);
    bus_event_pending = true;
}

// Inter-core shared data buffers (Core 1 updates, Core 0 streams to client)
volatile uint16_t shared_analog[4] = {0, 0, 0, 0};
volatile uint16_t shared_digital = 0;
// 'real' stores actual ADC hardware readings; 'mock' pushes 0 to the client when rails are disabled
volatile uint16_t shared_psu_real[4] = {0, 0, 0, 0};
volatile uint16_t shared_psu_mock[4] = {0, 0, 0, 0};
static volatile bool psu_5v_enabled = PICOLAB_HAS_ADJ_RAIL ? true : false;
static volatile bool psu_3v3_enabled = PICOLAB_HAS_ADJ_RAIL ? true : false;
static volatile bool psu_adj_enabled = PICOLAB_HAS_ADJ_RAIL ? true : false;
static volatile uint16_t psu_adj_voltage_mv = 1100;
static volatile uint16_t psu_adj_current_ma = 500;
static uint32_t uart_baud_hz = PICOLAB_UART_BAUD_RATE;
static uint32_t i2c0_baud_hz = 100000;
static bool i2c0_configured = false;


#if PICOLAB_HAS_ADJ_RAIL
static bool rail_i2c_configured = false;
#endif

#define I2C_SCAN_TIMEOUT_US 2000
#define I2C_XFER_TIMEOUT_US 5000

// Driver Output Strength Configuration
static bool set_gpio_drive_strength(uint8_t phys_pin, int drive_ma) {
    switch (drive_ma) {
        case 2:  gpio_set_drive_strength(phys_pin, GPIO_DRIVE_STRENGTH_2MA);  return true;
        case 4:  gpio_set_drive_strength(phys_pin, GPIO_DRIVE_STRENGTH_4MA);  return true;
        case 8:  gpio_set_drive_strength(phys_pin, GPIO_DRIVE_STRENGTH_8MA);  return true;
        case 12: gpio_set_drive_strength(phys_pin, GPIO_DRIVE_STRENGTH_12MA); return true;
        default: return false;
    }
}

// Reads JEDEC ID directly via SPI flash commands to determine total onboard storage size.
static void detect_flash_size(void) {
    // 0x9F is the standard JEDEC command for Read Identification
    uint8_t txbuf[4] = {0x9F, 0, 0, 0};
    uint8_t rxbuf[4] = {0};
    // Disable interrupts to prevent XIP (Execute-In-Place) collisions.
    uint32_t saved_irq = save_and_disable_interrupts();
    flash_do_cmd(txbuf, rxbuf, 4);
    restore_interrupts(saved_irq);
    // rxbuf[3] contains the capacity ID (e.g., 0x15 = 2MB, 0x16 = 4MB)
    if (rxbuf[3] >= 0x10 && rxbuf[3] <= 0x20) {
        global_flash_mb = (1 << rxbuf[3]) / (1024 * 1024);
    } else {
        global_flash_mb = PICO_FLASH_SIZE_BYTES / (1024 * 1024);
    }
}

// Queue system-level commands to ensure the network stack and USB interrupts have time to finish their current tasks
static bool queue_system_command(const char *cmd) {
    if (!cmd) return false;

    if (strcmp(cmd, "bootsel") == 0) {
        reboot_to_bootsel_pending = true;
        return true;
    }
    if (strcmp(cmd, "about") == 0) {
        send_sysinfo_pending = true;
        return true;
    }
    if (strcmp(cmd, "reboot") == 0) {
        reboot_normal_pending = true;
        return true;
    }

    return false;
}

static void spi0_apply_config(uint32_t baud_hz) {
    spi_init(PICOLAB_SPI_ID, baud_hz);
    gpio_set_function(PICOLAB_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICOLAB_SPI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICOLAB_SPI_MISO_PIN, GPIO_FUNC_SPI);
}

static void uart_apply_config(uint32_t baud_hz) {
    if (baud_hz == 0) return;
    uart_baud_hz = uart_set_baudrate(PICOLAB_UART_ID, baud_hz);
}

static void spi_prepare_cs_pin(uint gpio_pin) {
    gpio_init(gpio_pin);
    gpio_set_dir(gpio_pin, GPIO_OUT);
}

// ADC abstraction layer
static bool is_analog_channel_available(int channel) {
    return channel >= 0 && channel < NUM_ANALOG_CHANNELS && PICOLAB_ADC_INPUTS[channel] != PICOLAB_UNUSED_ADC;
}

static uint8_t logical_adc_to_hw_input(int channel) {
    if (!is_analog_channel_available(channel)) return 0;
    return PICOLAB_ADC_INPUTS[channel];
}

// Converts a bitmask of logical board channels into a bitmask of hardware ADC channels (used for DMA)
static uint32_t logical_adc_mask_to_hw_mask(uint8_t logical_mask) {
    uint32_t hw_mask = 0;
    for (int i = 0; i < NUM_ANALOG_CHANNELS; i++) {
        if ((logical_mask & (1u << i)) && is_analog_channel_available(i)) {
            hw_mask |= (1u << logical_adc_to_hw_input(i));
        }
    }
    return hw_mask;
}

static float analog_raw_to_volts(uint16_t raw) {
    return ((float)raw / 4095.0f) * PICOLAB_ADC_FULL_SCALE_V;
}

// GPIO Read helper abstraction. Handles RP2350 high GPIO pin registers (>32) correctly.
static inline bool safe_gpio_get(uint gpio) {
#if defined(PICO_RP2350)
    if (gpio >= 32) {
        return !!(sio_hw->gpio_hi_in & (1u << (gpio - 32)));
    }
#endif
    return !!(sio_hw->gpio_in & (1u << gpio));
}

// Sets the direction (input/output) of a managed GPIO pin. Handles level shifters or bare GPIO.
static void board_set_pin_direction(int pin_index, bool output_enabled) {
    if (pin_index < 0 || pin_index >= NUM_MANAGED_PINS) return;

    uint8_t phys_pin = MANAGED_PINS[pin_index];
    gpio_set_dir(phys_pin, output_enabled ? GPIO_OUT : GPIO_IN);

#if PICOLAB_HAS_EXT_DIR_CTRL
    uint8_t ctrl_pin = PICOLAB_DIR_CTRL_PINS[pin_index];
    if (ctrl_pin != PICOLAB_UNUSED_PIN) {
        gpio_put(ctrl_pin, output_enabled ? PICOLAB_DIR_OUT_LEVEL : !PICOLAB_DIR_OUT_LEVEL);
    }
#endif
}

static void board_set_pin_pull(int pin_index, bool pull_up) {
    if (pin_index < 0 || pin_index >= NUM_MANAGED_PINS) return;

    uint8_t phys_pin = MANAGED_PINS[pin_index];

#if PICOLAB_HAS_EXT_PULLUP_CTRL
    uint8_t ctrl_pin = PICOLAB_PULLUP_CTRL_PINS[pin_index];
    if (ctrl_pin != PICOLAB_UNUSED_PIN) {
        gpio_put(ctrl_pin, pull_up ? PICOLAB_PULLUP_ENABLE_LEVEL : !PICOLAB_PULLUP_ENABLE_LEVEL);
    }
    gpio_set_pulls(phys_pin, false, false);
#else
    gpio_set_pulls(phys_pin, pull_up, false);
#endif
}

// Captures the digital state of all managed pins simultaneously.
static uint8_t read_logic_bus_state(void) {
    uint8_t value = 0;
    for (int i = 0; i < NUM_MANAGED_PINS; i++) {
        if (safe_gpio_get(MANAGED_PINS[i])) value |= (1u << i);
    }
    return value;
}

#if PICOLAB_HAS_ADJ_RAIL
// MP28167 I2C PMIC Register Map
#define MP28167_REG_VREF_L 0x00u
#define MP28167_REG_VREF_H 0x01u
#define MP28167_REG_VREF_GO 0x02u
#define MP28167_REG_IOUT_LIM 0x03u
#define MP28167_REG_CTL1 0x04u

#define MP28167_CTL1_EN_BIT 0x80u
#define MP28167_CTL1_DEFAULT 0xF0u

#define MP28167_VREF_MIN_MV 80.0f
#define MP28167_VREF_MAX_MV 1637.0f
#define MP28167_VREF_LSB_MV 0.8f

#define MP28167_IOUT_MIN_MA 0u
#define MP28167_IOUT_MAX_MA 6350u
#define MP28167_IOUT_STEP_MA 50u

// Cache for CTL1 register to avoid unnecessary read-modify-write cycles
static uint8_t rail_ctl1_shadow = MP28167_CTL1_DEFAULT;

static void rail_i2c_apply_config(void) {
    i2c_deinit(PICOLAB_RAIL_I2C_ID);
    i2c_init(PICOLAB_RAIL_I2C_ID, PICOLAB_RAIL_I2C_BAUD_HZ);
    gpio_set_function(PICOLAB_RAIL_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICOLAB_RAIL_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICOLAB_RAIL_I2C_SDA_PIN);
    gpio_pull_up(PICOLAB_RAIL_I2C_SCL_PIN);
    rail_i2c_configured = true;
}

static bool rail_write_u8(uint8_t reg, uint8_t value) {
    if (!rail_i2c_configured) rail_i2c_apply_config();

    uint8_t payload[2] = {reg, value};
    int ret = i2c_write_timeout_us(PICOLAB_RAIL_I2C_ID, PICOLAB_RAIL_I2C_ADDR, payload, 2, false, I2C_XFER_TIMEOUT_US);
    return ret >= 0;
}

static bool rail_write_bytes(const uint8_t *payload, size_t length) {
    if (!rail_i2c_configured) rail_i2c_apply_config();

    int ret = i2c_write_timeout_us(PICOLAB_RAIL_I2C_ID, PICOLAB_RAIL_I2C_ADDR, payload, length, false, I2C_XFER_TIMEOUT_US);
    return ret >= 0;
}

static bool rail_hw_set_voltage_mv(uint16_t millivolts) {
    // Clamp requested voltage to safe hardware limits
    if (millivolts < PICOLAB_RAIL_VOUT_MIN_MV) millivolts = PICOLAB_RAIL_VOUT_MIN_MV;
    if (millivolts > PICOLAB_RAIL_VOUT_MAX_MV) millivolts = PICOLAB_RAIL_VOUT_MAX_MV;

    // Compensate for hardware feedback divider network
    float vref_mv = ((float)millivolts) / PICOLAB_RAIL_FB_GAIN;
    if (vref_mv < MP28167_VREF_MIN_MV) vref_mv = MP28167_VREF_MIN_MV;
    if (vref_mv > MP28167_VREF_MAX_MV) vref_mv = MP28167_VREF_MAX_MV;

    // Convert target VREF to 11-bit DAC code
    uint16_t vref_code = (uint16_t)((vref_mv / MP28167_VREF_LSB_MV) + 0.5f);
    if (vref_code > 2047u) vref_code = 2047u;

    uint8_t tx[4] = {
        MP28167_REG_VREF_L,
        (uint8_t)(vref_code & 0x07u),
        (uint8_t)((vref_code >> 3) & 0xFFu),
        0x01u // GO bit to apply new voltage
    };
    return rail_write_bytes(tx, sizeof(tx));
}

static bool rail_hw_set_current_ma(uint16_t milliamps) {
    if (milliamps < MP28167_IOUT_MIN_MA) milliamps = MP28167_IOUT_MIN_MA;
    if (milliamps > MP28167_IOUT_MAX_MA) milliamps = MP28167_IOUT_MAX_MA;
    
    // Calculate 7-bit current limit code (50mA per step)
    uint16_t code = (uint16_t)(milliamps / MP28167_IOUT_STEP_MA);
    if (code > 127u) code = 127u;
    return rail_write_u8(MP28167_REG_IOUT_LIM, (uint8_t)code);
}

/**
 * Brings up adjustable power regulator safely. Interrupts disabled temporarily during
 * reset to hit the tight initial I2C response window of the regulator PMIC.
 */
static void rail_safe_startup(void) {
    uint en = PICOLAB_RAIL_EN_PIN;
    
    // Hard-reset the PMIC by pulling enable pin low
    gpio_init(en);
    gpio_set_dir(en, GPIO_OUT);
    gpio_put(en, 0); 

    rail_i2c_apply_config();

    // Disable output in software before releasing hardware reset
    rail_ctl1_shadow &= (uint8_t)~MP28167_CTL1_EN_BIT;
    uint8_t ctl1_tx[2] = { MP28167_REG_CTL1, rail_ctl1_shadow };

    uint32_t irqs = save_and_disable_interrupts(); 
    gpio_put(en, 1);

    // Spam I2C bus to catch the PMIC exactly when it wakes up
    uint32_t start_time = time_us_32(); 
    while ((time_us_32() - start_time) < 5000) {
        if (i2c_write_timeout_us(PICOLAB_RAIL_I2C_ID, PICOLAB_RAIL_I2C_ADDR, ctl1_tx, 2, false, 500) >= 0) {
            break; 
        }
        busy_wait_us_32(10);
    }
    restore_interrupts(irqs); 
    
    rail_hw_set_voltage_mv(psu_adj_voltage_mv);
}

static void rail_hw_enable(bool enabled) {
    if (enabled) {
        // Temporarily raise current limit to handle startup inrush
        rail_hw_set_current_ma(2000);
        rail_hw_set_voltage_mv(psu_adj_voltage_mv);
        
        /* Internal DAC tracking stabilization delay to prevent startup voltage overshoot */
        busy_wait_us_32(10000); 
        
        rail_ctl1_shadow |= MP28167_CTL1_EN_BIT;
        (void)rail_write_u8(MP28167_REG_CTL1, rail_ctl1_shadow);
        
        // Wait for voltage to settle before applying user current limit
        busy_wait_us_32(2000);
        rail_hw_set_current_ma(psu_adj_current_ma);
    } else {
        rail_ctl1_shadow &= (uint8_t)~MP28167_CTL1_EN_BIT;
        (void)rail_write_u8(MP28167_REG_CTL1, rail_ctl1_shadow);
    }
}
#else
// Stub functions for variants without the adjustable rail hardware
static void rail_hw_enable(bool enabled) { (void)enabled; }
static bool rail_hw_set_voltage_mv(uint16_t millivolts) { (void)millivolts; return false; }
static bool rail_hw_set_current_ma(uint16_t milliamps) { (void)milliamps; return false; }
#endif

static void psu_refresh_telemetry(void) {
    if (!PICOLAB_HAS_ADJ_RAIL) {
        shared_psu_mock[0] = 0;
        shared_psu_mock[1] = 0;
        shared_psu_mock[2] = 0;
        shared_psu_mock[3] = 0;
        return;
    }

    // Zero out mock telemetry values if the respective rail is physically disabled
    shared_psu_mock[0] = psu_5v_enabled ? shared_psu_real[0] : 0;
    shared_psu_mock[1] = psu_3v3_enabled ? shared_psu_real[1] : 0;
    shared_psu_mock[2] = psu_adj_enabled ? shared_psu_real[2] : 0;
    shared_psu_mock[3] = psu_adj_enabled ? shared_psu_real[3] : 0;
}

static bool parse_on_off_value(const char *text, bool *value) {
    if (!text || !value) return false;
    if (strcmp(text, "1") == 0 || strcmp(text, "on") == 0 || strcmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "off") == 0 || strcmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool psu_set_rail_state(const char *output, bool enabled) {
    if (!PICOLAB_HAS_ADJ_RAIL) return false;
    if (!output) return false;

    // Route enable command to the requested rail
    if (strcmp(output, "5v") == 0) {
        psu_5v_enabled = enabled;
    } else if (strcmp(output, "3v3") == 0) {
        psu_3v3_enabled = enabled;
    } else if (strcmp(output, "adj") == 0) {
        psu_adj_enabled = enabled;
        rail_hw_enable(enabled);
    } else if (strcmp(output, "all") == 0) {
        psu_5v_enabled = enabled;
        psu_3v3_enabled = enabled;
        psu_adj_enabled = enabled;
        rail_hw_enable(enabled);
    } else {
        return false;
    }

    psu_refresh_telemetry();
    return true;
}

static bool psu_set_adj_voltage(float volts) {
    if (!PICOLAB_HAS_ADJ_RAIL) return false;
    
    // Bounds check and convert floats to millivolts for integer math
    if (volts < 0.0f) volts = 0.0f;
    if (volts > (PICOLAB_RAIL_VOUT_MAX_MV / 1000.0f)) volts = (PICOLAB_RAIL_VOUT_MAX_MV / 1000.0f);
    psu_adj_voltage_mv = (uint16_t)(volts * 1000.0f + 0.5f);
    
    if (psu_adj_voltage_mv < PICOLAB_RAIL_VOUT_MIN_MV) psu_adj_voltage_mv = PICOLAB_RAIL_VOUT_MIN_MV;
    if (psu_adj_voltage_mv > PICOLAB_RAIL_VOUT_MAX_MV) psu_adj_voltage_mv = PICOLAB_RAIL_VOUT_MAX_MV;
    
    rail_hw_set_voltage_mv(psu_adj_voltage_mv);
    psu_refresh_telemetry();
    return true;
}

static bool psu_set_adj_current(unsigned int milliamps) {
    if (!PICOLAB_HAS_ADJ_RAIL) return false;
    if (milliamps > 5000) milliamps = 5000;
    
    psu_adj_current_ma = (uint16_t)milliamps;
    rail_hw_set_current_ma(psu_adj_current_ma);
    psu_refresh_telemetry();
    return true;
}

// Oscilloscope buffer definitions
#define MAX_SCOPE_SAMPLES 4000
#define CAPTURE_BUF_SIZE 12000

// Internal buffers and DMA/Trigger state trackers
static uint16_t raw_buffer[CAPTURE_BUF_SIZE];
volatile uint16_t scope_buffer[MAX_SCOPE_SAMPLES];
volatile bool scope_running = false;
volatile bool scope_triggered = false;
volatile bool scope_data_ready = false;

volatile bool scope_rearm_requested = false;
volatile bool scope_stop_requested = false;

volatile int scope_trig_ch = 0;           
volatile float scope_trig_level_v = 1.5f;
volatile bool scope_trig_rising = true;
volatile float scope_timebase_ms = 10.0f;
volatile float scope_x_offset_ms = 0.0f;
static int scope_dma_chan = -1;
volatile uint8_t scope_active_mask = (1u << PICOLAB_ADC_ACTIVE_CHANNELS) - 1u;
volatile uint8_t scope_active_count = PICOLAB_ADC_ACTIVE_CHANNELS;
volatile int scope_current_samples = MAX_SCOPE_SAMPLES;
volatile int scope_mode = 0; // 0 = Normal, 1 = Single, 2 = Auto

static uint32_t hunt_index = 0;
static uint32_t trigger_index = 0;
static uint32_t stop_index = 0;
static bool trigger_armed = false;
static uint32_t auto_hunt_start_ms = 0;

// Logic analyzer buffer definitions
#if defined(PICO_RP2040) // The RP2040 does not have enough SRAM for the full array
    #define MAX_LOGIC_SAMPLES 20000
#else
    #define MAX_LOGIC_SAMPLES 80000
#endif
volatile uint8_t logic_buffer[MAX_LOGIC_SAMPLES];
volatile bool logic_running = false;
volatile bool logic_triggered = false;
volatile bool logic_data_ready = false;
volatile bool logic_rearm_requested = false;
volatile uint32_t logic_generation = 0;
volatile int logic_trig_pin = 0; 
volatile bool logic_trig_rising = true;
volatile float logic_sample_rate_hz = 10000000.0f;
static int logic_dma_chan = -1;
static uint logic_sm = 0;
static uint logic_pio_offset = 0;

#if PICOLAB_HAS_NEOPIXEL
static void status_led_show_connection_state(void) {
    uint32_t color;
    if (reboot_to_bootsel_pending) {
        color = PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0x00, 0x00, 0x12); // Blue for bootloader
    } else if (browser_connection_active) {
        color = PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0x00, 0x12, 0x00); // Green for active connection
    } else {
        color = PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0x12, 0x00, 0x00); // Red for disconnected/idle
    }
    (void)colored_status_led_set_on_with_color(color);
}
#else
static void status_led_write(bool on) {
    board_led_write(on);
}

static void status_led_show_connection_state(void) {
    status_led_write(reboot_to_bootsel_pending || browser_connection_active);
}
#endif

static void led_blinking_task(void) {
#if PICOLAB_HAS_NEOPIXEL
    static bool prev_bootsel = false;
    static bool prev_browser = false;
    static bool first_run = true;

    bool current_bootsel = reboot_to_bootsel_pending;
    bool current_browser = browser_connection_active;

    // Only update PIO if the state actually changes
    if (first_run || (current_bootsel != prev_bootsel) || (current_browser != prev_browser)) {
        status_led_show_connection_state();
        prev_bootsel = current_bootsel;
        prev_browser = current_browser;
        first_run = false;
    }
#else
    static uint32_t start_ms = 0;
    static bool led_state = false;
    
    if (to_ms_since_boot(get_absolute_time()) - start_ms < blink_interval_ms) return;
    
    start_ms += blink_interval_ms;
    status_led_write(led_state);
    led_state = !led_state;
#endif
}

// RFC 4648 Base64 encoding table
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *src, size_t len, char *out) {
    size_t i = 0, j = 0;
    // Process 3-byte blocks into 4-byte Base64 strings
    for (; i < len - 2; i += 3) {
        out[j++] = b64_table[(src[i] >> 2) & 0x3F];
        out[j++] = b64_table[((src[i] & 0x3) << 4) | ((src[i + 1] & 0xF0) >> 4)];
        out[j++] = b64_table[((src[i + 1] & 0xF) << 2) | ((src[i + 2] & 0xC0) >> 6)];
        out[j++] = b64_table[src[i + 2] & 0x3F];
    }
    // Handle padding for remaining bytes
    if (i < len) {
        out[j++] = b64_table[(src[i] >> 2) & 0x3F];
        if (i == (len - 1)) {
            out[j++] = b64_table[((src[i] & 0x3) << 4)]; 
            out[j++] = '=';
        } else {
            out[j++] = b64_table[((src[i] & 0x3) << 4) | ((src[i + 1] & 0xF0) >> 4)];
            out[j++] = b64_table[((src[i + 1] & 0xF) << 2)];
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return j;
}

static const char *query_value(int param_count, char *params[], char *values[], const char *key) {
    for (int i = 0; i < param_count; i++) {
        if (strcmp(params[i], key) == 0) return values[i];
    }
    return NULL;
}

static int query_value_int(int param_count, char *params[], char *values[], const char *key, int fallback) {
    const char *val = query_value(param_count, params, values, key);
    return val ? atoi(val) : fallback;
}

static float query_value_float(int param_count, char *params[], char *values[], const char *key, float fallback) {
    const char *val = query_value(param_count, params, values, key);
    return val ? atof(val) : fallback;
}

static bool is_valid_pin_index(int pin_index) {
    return pin_index >= 0 && pin_index < NUM_MANAGED_PINS;
}

static uint8_t logic_remap_lut[256];

static void init_logic_remap_lut(void) {
#if PICOLAB_HW_TARGET == HW_PICO
    // Pico GPIO mapping is 1:1, no remapping required
#else
    for (int raw = 0; raw < 256; raw++) {
        uint8_t remapped = 0;
        for (int d = 0; d < NUM_MANAGED_PINS; d++) {
            int bit_pos = MANAGED_PINS[d] - PICOLAB_LOGIC_BASE_PIN;
            if (bit_pos >= 0 && bit_pos < 8) {
                if (raw & (1u << bit_pos)) {
                    remapped |= (1u << d);
                }
            }
        }
        logic_remap_lut[raw] = remapped;
    }
#endif
}

// logic analyzer initialization
void logic_hardware_init(void) {
    init_logic_remap_lut();
    logic_pio_offset = pio_add_program(pio1, &logic_analyzer_program);
    logic_sm = pio_claim_unused_sm(pio1, true);
    logic_dma_chan = dma_claim_unused_channel(true);
}

// Run this exactly when the trigger fires
void arm_logic_analyzer_hardware(uint base_pin, float freq_hz) {
    pio_sm_set_enabled(pio1, logic_sm, false);
    pio_sm_clear_fifos(pio1, logic_sm);
    pio_sm_restart(pio1, logic_sm);
    
    pio_sm_config c = logic_analyzer_program_get_default_config(logic_pio_offset);
    sm_config_set_in_pins(&c, base_pin);
    sm_config_set_in_shift(&c, false, true, 8);
    
    float div = (float)clock_get_hz(clk_sys) / freq_hz;
    if (div < 1.0f) div = 1.0f; // Prevent divider from hitting 0
    sm_config_set_clkdiv(&c, div);
    
    pio_sm_init(pio1, logic_sm, logic_pio_offset, &c);

    dma_channel_config dmac = dma_channel_get_default_config(logic_dma_chan);
    channel_config_set_transfer_data_size(&dmac, DMA_SIZE_8);
    channel_config_set_read_increment(&dmac, false);
    channel_config_set_write_increment(&dmac, true);
    channel_config_set_dreq(&dmac, pio_get_dreq(pio1, logic_sm, false));

    dma_channel_configure(logic_dma_chan, &dmac,
        logic_buffer,             
        &pio1->rxf[logic_sm],     
        MAX_LOGIC_SAMPLES,        
        true                      
    );
}

// Core 1 logic analyzer routine. Waits for edge trigger condition before capturing sample buffer.
void logic_task(void) {
    uint trig_pin = MANAGED_PINS[logic_trig_pin];
    static bool logic_armed = false;

    if (logic_rearm_requested) {
        logic_rearm_requested = false;
        if (dma_channel_is_busy(logic_dma_chan)) {
            dma_channel_abort(logic_dma_chan);
        }
        pio_sm_set_enabled(pio1, logic_sm, false);
        pio_sm_clear_fifos(pio1, logic_sm);
        pio_sm_restart(pio1, logic_sm);
        logic_triggered = false;
        logic_data_ready = false;
        logic_armed = false;
    }

    if (!logic_running) {
        if (dma_channel_is_busy(logic_dma_chan)) {
            dma_channel_abort(logic_dma_chan);
            pio_sm_set_enabled(pio1, logic_sm, false);
        }
        logic_triggered = false;
        logic_armed = false;
        return;
    }
    if (logic_data_ready) return;

    // 1. Pre-configure hardware math and DMA
    if (!logic_armed) {
        arm_logic_analyzer_hardware(PICOLAB_LOGIC_BASE_PIN, logic_sample_rate_hz);
        logic_armed = true;
    }

    if (!logic_triggered) {
        bool current_state = safe_gpio_get(trig_pin);

        // Require the signal to be in the opposite state before arming the edge wait.
        bool pre_state_met = (logic_trig_rising && !current_state) ||
                             (!logic_trig_rising && current_state);

        if (pre_state_met) {
            pio_sm_set_enabled(pio1, logic_sm, false);
            pio_sm_clear_fifos(pio1, logic_sm);
            pio_sm_restart(pio1, logic_sm);

            pio_sm_exec(pio1, logic_sm, pio_encode_wait_gpio(logic_trig_rising, trig_pin));
            pio_sm_set_enabled(pio1, logic_sm, true);
            logic_triggered = true;
        }
    } 
    else {
        // 3. Poll DMA completion
        if (!dma_channel_is_busy(logic_dma_chan)) {
            pio_sm_set_enabled(pio1, logic_sm, false);

#if PICOLAB_HW_TARGET != HW_PICO
            for (int i = 0; i < MAX_LOGIC_SAMPLES; i++) {
                logic_buffer[i] = logic_remap_lut[logic_buffer[i]];
            }
#endif
            logic_data_ready = true;
            logic_running = false; 
            logic_triggered = false;
            logic_armed = false;
        }
    }
}

// Oscilloscope sampling task executing on Core 1.
// Evaluates trigger levels against incoming continuous DMA ring-buffer acquisitions.
static void scope_start_capture(void) {
    adc_run(false);
    adc_fifo_drain();
    adc_set_round_robin(0);

    int first_ch = 0;
    for (int channel = 0; channel < NUM_ANALOG_CHANNELS; channel++) {
        if (scope_active_mask & (1u << channel)) {
            first_ch = logical_adc_to_hw_input(channel);
            break;
        }
    }

    adc_select_input(first_ch);
    if (scope_active_count > 1) {
        adc_set_round_robin(logical_adc_mask_to_hw_mask(scope_active_mask));
    }

    dma_channel_set_write_addr(scope_dma_chan, raw_buffer, false);
    dma_channel_set_trans_count(scope_dma_chan, CAPTURE_BUF_SIZE, true);
    adc_run(true);

    hunt_index = 0;
    trigger_armed = false;
    auto_hunt_start_ms = to_ms_since_boot(get_absolute_time());
}

void scope_task(void) {
    if (!scope_running || scope_data_ready) return;

    // Calculate current write head in the cyclic DMA buffer
    uint32_t current_index = CAPTURE_BUF_SIZE - dma_channel_hw_addr(scope_dma_chan)->transfer_count;

    if (!dma_channel_is_busy(scope_dma_chan) && !scope_triggered) {
        if (hunt_index >= current_index) {
            static uint32_t last_capture_ms = 0;
            // Introduce artificial dead-time to avoid overwhelming the web UI
            if (to_ms_since_boot(get_absolute_time()) - last_capture_ms < 30) return; 
            last_capture_ms = to_ms_since_boot(get_absolute_time());

            scope_start_capture();
            return; 
        }
    }

    // Determine target channel position within the interleaved data
    int target_ch_offset = 0;
    int active_idx = 0;
    for(int b=0; b<NUM_ANALOG_CHANNELS; b++) {
        if(scope_active_mask & (1<<b)) {
            if(b == scope_trig_ch) target_ch_offset = active_idx;
            active_idx++;
        }
    }

    if (!scope_triggered) {
        float total_time_ms = scope_timebase_ms * 10.0f;
        float offset_fraction = scope_x_offset_ms / total_time_ms;
        
        // Calculate pre-trigger sample count based on horizontal offset
        int pre_trigger = (int)(scope_current_samples * (0.5f - offset_fraction));
        
        // Constrain pre-trigger to valid buffer boundaries
        if (pre_trigger < scope_active_count) pre_trigger = scope_active_count;
        if (pre_trigger >= scope_current_samples - scope_active_count) {
            pre_trigger = scope_current_samples - scope_active_count;
        }

        // Snap pre-trigger alignment to channel frame boundary
        pre_trigger -= (pre_trigger % scope_active_count); 
        if (pre_trigger == 0) pre_trigger = scope_active_count;

        // Apply hysteresis to trigger thresholds to reject baseline noise
        uint16_t thresh = (uint16_t)((scope_trig_level_v / PICOLAB_ADC_FULL_SCALE_V) * 4095.0f);
        uint16_t hysteresis = 100; 
        uint16_t thresh_high = (thresh + hysteresis < 4095) ? thresh + hysteresis : 4095;
        uint16_t thresh_low  = (thresh > hysteresis) ? thresh - hysteresis : 0;

        while (hunt_index < current_index) {
            if ((hunt_index % scope_active_count) == target_ch_offset) {
                uint16_t val = raw_buffer[hunt_index];
                bool is_triggered = false;
                
                // Arm trigger when signal crosses hysteresis threshold, fire on main threshold
                if (scope_trig_rising) {
                    if (val <= thresh_low) trigger_armed = true;
                    if (trigger_armed && val >= thresh_high) is_triggered = true;
                } else {
                    if (val >= thresh_high) trigger_armed = true;
                    if (trigger_armed && val <= thresh_low) is_triggered = true;
                }

                if (is_triggered) {
                    // Only accept trigger if we have collected enough pre-trigger history
                    if (hunt_index >= pre_trigger) {
                        scope_triggered = true;
                        trigger_index = hunt_index - pre_trigger;
                        trigger_index -= (trigger_index % scope_active_count);
                        stop_index = trigger_index + scope_current_samples;

                        // Abort if acquisition will run past end of capture buffer
                        if (stop_index > CAPTURE_BUF_SIZE) {
                            adc_run(false);
                            dma_channel_abort(scope_dma_chan);
                            adc_set_round_robin(0); 
                            scope_triggered = false;
                            trigger_armed = false;
                            hunt_index = CAPTURE_BUF_SIZE;
                        }
                        break;
                    } else {
                        trigger_armed = false;
                    }
                }
            }
            hunt_index++;
        }

        // Auto mode timeout handler (forces trigger if no edges found)
        if (!scope_triggered && scope_mode == 2) {
            uint32_t timeout_ms = (uint32_t)(scope_timebase_ms * 10.0f) + 100;
            if (timeout_ms < 200) timeout_ms = 200; 

            bool buffer_full = (!dma_channel_is_busy(scope_dma_chan)) && (current_index >= CAPTURE_BUF_SIZE);
            
            if ((to_ms_since_boot(get_absolute_time()) - auto_hunt_start_ms > timeout_ms) || buffer_full) {
                if (current_index >= scope_current_samples) {
                    scope_triggered = true;
                    trigger_index = current_index - scope_current_samples;
                    trigger_index -= (trigger_index % scope_active_count);
                    stop_index = current_index; 
                }
            }
        }
    } else {
        // Wait for DMA to reach calculated stop index
        if (current_index >= stop_index || !dma_channel_is_busy(scope_dma_chan)) {
            adc_run(false);
            dma_channel_abort(scope_dma_chan);
            adc_set_round_robin(0); 

            // Copy captured slice into finalized display buffer
            memcpy((void*)scope_buffer, (void*)&raw_buffer[trigger_index], scope_current_samples * sizeof(uint16_t));

            scope_data_ready = true;
            hunt_index = CAPTURE_BUF_SIZE; 
        }
    }
}

/**
 * Entry point for Core 1. Dedicated to high-frequency acquisition (ADC/DMA & Logic sampling)
 * to keep signal capturing decoupled from network processing on Core 0.
 */
void core1_entry(void) {
    // Overclock ADC to 192MHz for higher sampling rates
    clock_configure(clk_adc,
                    0,
                    CLOCKS_CLK_ADC_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    192000000,
                    192000000);
    adc_init();
    
    // Initialize standard ADC pins
    for (int i = 0; i < NUM_ANALOG_CHANNELS; i++) {
        if (is_analog_channel_available(i) && PICOLAB_ADC_GPIOS[i] != PICOLAB_UNUSED_PIN) {
            adc_gpio_init(PICOLAB_ADC_GPIOS[i]);
        }
    }

#if PICOLAB_HAS_ADJ_RAIL
    // Initialize power supply telemetry ADC pins
    adc_gpio_init(PSU_ADC_PIN_ADJ_I);
    adc_gpio_init(PSU_ADC_PIN_3V3_I);
    adc_gpio_init(PSU_ADC_PIN_5V_I);
    adc_gpio_init(PSU_ADC_PIN_ADJ_V);
#endif

    // Setup DMA channel for continuous oscilloscope capture
    scope_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(scope_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    
    dma_channel_configure(scope_dma_chan, &c,
        raw_buffer,      
        &adc_hw->fifo,   
        MAX_SCOPE_SAMPLES, 
        false            
    );
    adc_fifo_setup(true, true, 1, false, false);

    // init logic analyzer
    logic_hardware_init();

    // Main routine (Core 1)
    while(1) {
        if (scope_rearm_requested) {
            // Reset scope state and clear DMA
            scope_rearm_requested = false;
            adc_run(false);
            dma_channel_abort(scope_dma_chan);
            adc_set_round_robin(0);
            
            scope_triggered = false;
            scope_data_ready = false;
            trigger_index = 0;
            stop_index = 0;
            trigger_armed = false;
            
            // Calculate required ADC clock divider based on timebase
            float total_time_s = (scope_timebase_ms * 10.0f) / 1000.0f;
            float fs = MAX_SCOPE_SAMPLES / total_time_s; 
            scope_current_samples = MAX_SCOPE_SAMPLES;

            if (fs >= 2000000.0f) {
                fs = 2000000.0f; 
                scope_current_samples = (int)(fs * total_time_s);
                scope_current_samples -= (scope_current_samples % scope_active_count); 
                adc_set_clkdiv(0); 
            } else {
                adc_set_clkdiv(192000000.0f / fs); 
            }

            scope_start_capture();
        } else if (scope_stop_requested) {
            scope_stop_requested = false;
            adc_run(false);
            dma_channel_abort(scope_dma_chan);
            adc_set_round_robin(0);
        }

        scope_task();
        logic_task();

        // Update standard telemetry only when scope or logic analyzer are not capturing
        if (!scope_running && !logic_running) {
            for(int i = 0; i < NUM_ANALOG_CHANNELS; i++) {
                if (!is_analog_channel_available(i)) {
                    shared_analog[i] = 0;
                    continue;
                }
                adc_select_input(logical_adc_to_hw_input(i));
                shared_analog[i] = adc_read();
            }

#if PICOLAB_HAS_ADJ_RAIL
            float raw;
            
            // Read power supply voltages and currents
            adc_select_input(PSU_ADC_PIN_5V_I - 40);
            raw = (adc_read() / 4095.0f) * 3.0f;
            shared_psu_real[0] = (uint16_t)(raw * PSU_SCALE_CURRENT * 1000.0f);

            adc_select_input(PSU_ADC_PIN_3V3_I - 40);
            raw = (adc_read() / 4095.0f) * 3.0f;
            shared_psu_real[1] = (uint16_t)(raw * PSU_SCALE_CURRENT * 1000.0f);

            adc_select_input(PSU_ADC_PIN_ADJ_I - 40);
            raw = (adc_read() / 4095.0f) * 3.0f;
            shared_psu_real[2] = (uint16_t)(raw * PSU_SCALE_CURRENT * 1000.0f);

            adc_select_input(PSU_ADC_PIN_ADJ_V - 40);
            raw = (adc_read() / 4095.0f) * 3.0f;
            shared_psu_real[3] = (uint16_t)(raw * PSU_SCALE_VOLTAGE * 1000.0f);
#endif
        }
        
        shared_digital = read_logic_bus_state();
        if (!logic_running) {
            sleep_us(10);
        }
    }
}

/* TinyUSB CDC & USB Network Callbacks */

/**
 * Detects 1200 baud connection toggle (Arduino-style bootloader reset trick).
 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding) {
    (void) itf;
    if (p_line_coding->bit_rate == 1200) {
        reboot_to_bootsel_pending = true;
    }
}

// Push pending network packets to the USB stack
void usb_network_flush_tx(void) {
    if (tud_network_can_xmit(1500)) {
        if (tx_pbuf_head != tx_pbuf_tail) {
            struct pbuf *q = tx_pbuf_queue[tx_pbuf_tail];
            tx_pbuf_tail = (tx_pbuf_tail + 1) % TX_PBUF_QUEUE_LEN;
            tud_network_xmit(q, 0);
        }
    }
}

// Queue packet for USB transmission and increment reference counter
static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void) netif;
    if (!tud_ready()) return ERR_USE;

    int next_head = (tx_pbuf_head + 1) % TX_PBUF_QUEUE_LEN;
    if (next_head == tx_pbuf_tail) return ERR_USE; 

    /* Increment reference counter to avoid deep memory copies during stack bridging */
    pbuf_ref(p); 
    tx_pbuf_queue[tx_pbuf_head] = p;
    tx_pbuf_head = next_head;

    return ERR_OK; 
}

// ARP resolution for IPv4 packets
static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

// Configure lwIP network interface parameters
static err_t netif_init_cb(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->linkoutput = linkoutput_fn;
    netif->output = ip4_output_fn;
    return ERR_OK;
}

// Bring up the lwIP stack and default network interface
static void init_lwip(void) {
    struct netif *netif = &netif_data;
    lwip_init();
    
    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    netif->hwaddr[5] ^= 0x01;

    netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ethernet_input);
    netif_set_default(netif);
    netif_set_link_up(netif);
}

// Resolve local domains to the Pico's IP address
static bool dns_query_proc(const char *name, ip4_addr_t *addr) {
    if (strcmp(name, "pico.lab") == 0 || strcmp(name, "www.pico.lab") == 0) {
        *addr = ipaddr;
        return true;
    }
    if (strncmp(name, "wpad", 4) == 0) {
        *addr = ipaddr;
        return true;
    }
    return false;
}

void tud_network_init_cb(void) {
    tx_pbuf_head = 0;
    tx_pbuf_tail = 0;
}

// Pass incoming USB network packets to lwIP
bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    struct netif *netif = &netif_data;
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p == NULL) {
            tud_network_recv_renew();
            return true; 
        }
        pbuf_take(p, src, size);
        if (netif->input(p, netif) != ERR_OK) {
            pbuf_free(p);
        }
        tud_network_recv_renew();
    }
    return true;
}

// Copy packet data to USB transmit buffer
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *) ref;
    (void) arg;
    uint16_t len = pbuf_copy_partial(p, dst, p->tot_len, 0);
    pbuf_free(p); 
    return len;
}

/* HTTP CGI Handlers for web dashboard interactions */

// HTTP CGI: Set GPIO pin high or low
const char * cgi_toggle_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    int target_pin = query_value_int(iNumParams, pcParam, pcValue, "pin", -1);
    int target_state = query_value_int(iNumParams, pcParam, pcValue, "state", -1);

    if (is_valid_pin_index(target_pin) && target_state >= 0) {
        uint8_t phys_pin = MANAGED_PINS[target_pin];
        board_set_pin_direction(target_pin, true);
        gpio_put(phys_pin, target_state);
    }
    return "/ok.json";
}

// Configure pin as input, output, or PWM
static void config_pin_mode(int pin_index, uint8_t phys_pin, const char* mode) {
    if (strcmp(mode, "in") == 0) {
        gpio_init(phys_pin);
        board_set_pin_direction(pin_index, false);
    } else if (strcmp(mode, "out") == 0) {
        gpio_init(phys_pin);
        board_set_pin_direction(pin_index, true);
    } else if (strcmp(mode, "pwm") == 0) {
        gpio_set_function(phys_pin, GPIO_FUNC_PWM);
        board_set_pin_direction(pin_index, true);
        uint slice_num = pwm_gpio_to_slice_num(phys_pin);
        pwm_set_wrap(slice_num, 10000); 
        pwm_set_chan_level(slice_num, pwm_gpio_to_channel(phys_pin), 0); 
        pwm_set_clkdiv(slice_num, 12.5f); 
        pwm_set_enabled(slice_num, true);
    }
}

static void config_pin_pull(int pin_index, uint8_t phys_pin, bool pull_up) {
    (void)phys_pin;
    board_set_pin_pull(pin_index, pull_up);
}

// HTTP CGI: Apply advanced pin configurations (mode, PWM, drive strength, pull)
const char * cgi_config_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    int target_pin = query_value_int(iNumParams, pcParam, pcValue, "pin", -1);
    char mode[10] = {0};
    int pwm_duty = query_value_int(iNumParams, pcParam, pcValue, "pwm_duty", -1);
    int drive = query_value_int(iNumParams, pcParam, pcValue, "drive", -1);
    int pwm_freq = query_value_int(iNumParams, pcParam, pcValue, "pwm_freq", -1);

    const char *mode_param = query_value(iNumParams, pcParam, pcValue, "mode");
    const char *pull_param = query_value(iNumParams, pcParam, pcValue, "pull");
    if (mode_param) strncpy(mode, mode_param, sizeof(mode) - 1);

    bool pull_up = false;
    if (pull_param && strcmp(pull_param, "up") == 0) {
        pull_up = true;
    }

    if (is_valid_pin_index(target_pin)) {
        uint8_t phys_pin = MANAGED_PINS[target_pin];

        if (strlen(mode) > 0) config_pin_mode(target_pin, phys_pin, mode);
        if (pull_param) config_pin_pull(target_pin, phys_pin, pull_up);

        if (pwm_duty >= 0 && pwm_duty <= 100) {
            uint slice_num = pwm_gpio_to_slice_num(phys_pin);
            pwm_set_chan_level(slice_num, pwm_gpio_to_channel(phys_pin), pwm_duty * 100);
        }
        if (drive > 0) {
            set_gpio_drive_strength(phys_pin, drive);
        }
        if (pwm_freq > 0) {
            uint slice_num = pwm_gpio_to_slice_num(phys_pin);
            float div = (float)clock_get_hz(clk_sys) / (pwm_freq * 10000.0f);
            if (div < 1.0f) div = 1.0f;
            if (div > 255.9f) div = 255.9f;
            pwm_set_clkdiv(slice_num, div);
        }
    }
    return "/ok.json";
}

// HTTP CGI: Queue system commands like reboot
const char * cgi_system_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *cmd = query_value(iNumParams, pcParam, pcValue, "cmd");
    queue_system_command(cmd);
    return "/ok.json";
}

// HTTP CGI: Apply the physical UART baud rate
const char * cgi_uart_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *action = query_value(iNumParams, pcParam, pcValue, "action");
    if (action && strcmp(action, "setup") == 0) {
        int baud = query_value_int(iNumParams, pcParam, pcValue, "baud", 0);
        if (baud > 0) uart_apply_config((uint32_t)baud);
    }
    return "/ok.json";
}

// HTTP CGI: Control adjustable power supply rails
const char * cgi_power_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *output = query_value(iNumParams, pcParam, pcValue, "output");
    const char *state_val = query_value(iNumParams, pcParam, pcValue, "state");
    const char *voltage_val = query_value(iNumParams, pcParam, pcValue, "voltage");
    const char *limit_val = query_value(iNumParams, pcParam, pcValue, "limit");

    if (output && state_val) {
        bool enabled = false;
        if (parse_on_off_value(state_val, &enabled)) {
            psu_set_rail_state(output, enabled);
        }
    }

    if (output && strcmp(output, "adj") == 0) {
        if (voltage_val) {
            psu_set_adj_voltage((float)atof(voltage_val));
        }
        if (limit_val) {
            unsigned int limit = 0;
            if (parse_uint_auto(limit_val, &limit)) {
                psu_set_adj_current(limit);
            }
        }
    }

    if (output && strcmp(output, "all") == 0 && !state_val && !voltage_val && !limit_val) {
        psu_refresh_telemetry();
    }

    return "/ok.json";
}

// HTTP CGI: Update oscilloscope trigger and timebase settings
const char * cgi_scope_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *run_val = query_value(iNumParams, pcParam, pcValue, "run");
    bool rearm = false;
    if (run_val) {
        scope_running = atoi(run_val);
        rearm = true;
    }

    scope_trig_ch = query_value_int(iNumParams, pcParam, pcValue, "trig_ch", scope_trig_ch);
    scope_trig_level_v = query_value_float(iNumParams, pcParam, pcValue, "trig_level", scope_trig_level_v);
    scope_trig_rising = query_value_int(iNumParams, pcParam, pcValue, "trig_edge", scope_trig_rising);
    scope_timebase_ms = query_value_float(iNumParams, pcParam, pcValue, "tb", scope_timebase_ms);
    scope_x_offset_ms = query_value_float(iNumParams, pcParam, pcValue, "x_off", scope_x_offset_ms);
    scope_mode = query_value_int(iNumParams, pcParam, pcValue, "mode", scope_mode);

    if (!is_analog_channel_available(scope_trig_ch)) scope_trig_ch = 0;
    if (scope_trig_level_v < 0.0f) scope_trig_level_v = 0.0f;
    if (scope_trig_level_v > PICOLAB_ADC_FULL_SCALE_V) scope_trig_level_v = PICOLAB_ADC_FULL_SCALE_V;

    const char *mask_val = query_value(iNumParams, pcParam, pcValue, "mask");
    if (mask_val) {
        uint8_t allowed_mask = (1u << NUM_ANALOG_CHANNELS) - 1u;
        scope_active_mask = (uint8_t)(atoi(mask_val) & allowed_mask);
        if (is_analog_channel_available(scope_trig_ch)) {
            scope_active_mask |= (uint8_t)(1u << scope_trig_ch);
        }
        scope_active_count = 0;
        for (int b = 0; b < NUM_ANALOG_CHANNELS; b++) {
            if (scope_active_mask & (1 << b)) scope_active_count++;
        }
        if (scope_active_count == 0) {
            scope_active_mask = 1;
            scope_active_count = 1;
        }
    }

    if (scope_running && rearm) {
        scope_stop_requested = false;
        scope_rearm_requested = true;
    } else if (!scope_running) {
        scope_rearm_requested = false;
        scope_stop_requested = true;
    }
    return "/ok.json";
}

// HTTP CGI: Update logic analyzer settings
const char * cgi_logic_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    logic_trig_pin = query_value_int(iNumParams, pcParam, pcValue, "trig_pin", logic_trig_pin);
    if (!is_valid_pin_index(logic_trig_pin)) logic_trig_pin = 0;
    logic_trig_rising = query_value_int(iNumParams, pcParam, pcValue, "trig_edge", logic_trig_rising);
    logic_sample_rate_hz = query_value_float(iNumParams, pcParam, pcValue, "rate", logic_sample_rate_hz);
    logic_generation = (uint32_t)query_value_int(iNumParams, pcParam, pcValue, "gen", (int)logic_generation);

    const char *run_val = query_value(iNumParams, pcParam, pcValue, "run");
    if (run_val) {
        int new_run = atoi(run_val);
        if (new_run) {
            for (int i = 0; i < NUM_MANAGED_PINS; i++) {
                board_set_pin_direction(i, false);
            }
            logic_data_ready = false;
            logic_triggered = false;
            logic_running = true;
            __compiler_memory_barrier();
            logic_rearm_requested = true;
        } else {
            logic_running = false;
            logic_data_ready = false;
            logic_rearm_requested = false;
        }
    }

    return "/ok.json";
}

// Convert hex string to byte array
static size_t parse_hex_string(const char* hex, uint8_t* out_buf, size_t max_len) {
    size_t len = 0;
    while (hex && hex[0] && hex[1] && len < max_len) {
        uint8_t byte = 0;
        for (int i = 0; i < 2; i++) {
            char c = hex[i];
            byte <<= 4;
            if (c >= '0' && c <= '9') byte |= (c - '0');
            else if (c >= 'a' && c <= 'f') byte |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte |= (c - 'A' + 10);
        }
        out_buf[len++] = byte;
        hex += 2;
    }
    return len;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex_string_strict(const char *hex, uint8_t *out_buf, size_t max_len, size_t *out_len) {
    *out_len = 0;
    if (!hex || !hex[0]) return true;

    while (hex[0]) {
        int hi = hex_nibble(hex[0]);
        int lo = hex_nibble(hex[1]);
        if (hex[1] == '\0' || hi < 0 || lo < 0) {
            return false;
        }
        if (*out_len >= max_len) {
            return false;
        }
        out_buf[(*out_len)++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }

    return true;
}

static void i2c0_apply_config(uint32_t baud_hz) {
    i2c_deinit(PICOLAB_I2C0_ID);
    i2c_init(PICOLAB_I2C0_ID, baud_hz);
    gpio_set_function(PICOLAB_I2C0_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICOLAB_I2C0_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICOLAB_I2C0_SDA_PIN);
    gpio_pull_up(PICOLAB_I2C0_SCL_PIN);
    i2c0_baud_hz = baud_hz;
    i2c0_configured = true;
}

// Toggles SCL pin 9 times to unstick frozen target peripherals keeping the SDA bus low.
static void i2c0_bus_unstick(void) {
    gpio_set_function(PICOLAB_I2C0_SDA_PIN, GPIO_FUNC_SIO);
    gpio_set_function(PICOLAB_I2C0_SCL_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(PICOLAB_I2C0_SDA_PIN, GPIO_OUT);
    gpio_set_dir(PICOLAB_I2C0_SCL_PIN, GPIO_OUT);
    
    // Manually toggle SCL to free stuck I2C devices
    gpio_put(PICOLAB_I2C0_SDA_PIN, 1);
    gpio_put(PICOLAB_I2C0_SCL_PIN, 1);
    sleep_us(5);

    for (int i = 0; i < 9; i++) {
        gpio_put(PICOLAB_I2C0_SCL_PIN, 0);
        sleep_us(5);
        gpio_put(PICOLAB_I2C0_SCL_PIN, 1);
        sleep_us(5);
    }

    gpio_put(PICOLAB_I2C0_SDA_PIN, 0);
    sleep_us(5);
    gpio_put(PICOLAB_I2C0_SCL_PIN, 1);
    sleep_us(5);
    gpio_put(PICOLAB_I2C0_SDA_PIN, 1);
    sleep_us(5);
}

static void i2c0_recover_after_error(void) {
    i2c0_bus_unstick();
    i2c0_apply_config(i2c0_baud_hz ? i2c0_baud_hz : 100000);
}

// HTTP CGI: I2C bus setup, scanning, and transfers
const char * cgi_i2c_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *action = query_value(iNumParams, pcParam, pcValue, "action");
    if (!action) return "/ok.json";

    if (strcmp(action, "setup") == 0) {
        int baud = query_value_int(iNumParams, pcParam, pcValue, "baud", 100000);
        i2c0_apply_config((uint32_t)baud);
        queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C setup at %d Hz on GPIO%d/GPIO%d\"}", baud, PICOLAB_I2C0_SDA_PIN, PICOLAB_I2C0_SCL_PIN);
    } 
    else if (strcmp(action, "scan") == 0) {
        if (!i2c0_configured) i2c0_apply_config(i2c0_baud_hz);

        char addrs[460] = "[";
        int count = 0;
        for (int addr = 0x08; addr < 0x78; ++addr) {
            uint8_t rxdata;
            int sret = i2c_read_timeout_us(PICOLAB_I2C0_ID, addr, &rxdata, 1, false, I2C_SCAN_TIMEOUT_US);
            if (sret >= 0) {
                if (count > 0) strcat(addrs, ",");
                sprintf(addrs + strlen(addrs), "%d", addr);
                count++;
            }
        }
        strcat(addrs, "]");
        queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C scan done (%d device%s)\",\"found\":%s}",
                             count, (count == 1) ? "" : "s", addrs);
    }
    else {
        if (!i2c0_configured) i2c0_apply_config(i2c0_baud_hz);

        int addr = query_value_int(iNumParams, pcParam, pcValue, "addr", 0);
        const char *data_str = query_value(iNumParams, pcParam, pcValue, "data");
        uint8_t buf[128];
        size_t len = 0;

        if (addr < 0x08 || addr > 0x77) {
            queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"Invalid I2C address\",\"err\":-2}");
            return "/ok.json";
        }

        if (!parse_hex_string_strict(data_str, buf, sizeof(buf), &len)) {
            queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"Invalid hex payload (use full byte pairs like 0A1B)\",\"err\":-3}");
            return "/ok.json";
        }

        if (strcmp(action, "write") == 0) {
            int ret = i2c_write_timeout_us(PICOLAB_I2C0_ID, addr, buf, len, false, I2C_XFER_TIMEOUT_US);
            if (ret >= 0) {
                queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C write ok\",\"bytes\":%d}", ret);
            } else {
                i2c0_recover_after_error();
                queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C write failed\",\"err\":%d}", ret);
            }
        } 
        else if (strcmp(action, "read") == 0 || strcmp(action, "writeread") == 0) {
            int rlen = query_value_int(iNumParams, pcParam, pcValue, "rlen", 1);
            if (rlen <= 0) rlen = 1;
            if (rlen > sizeof(buf)) rlen = sizeof(buf);
            
            if (strcmp(action, "writeread") == 0) {
                if (len == 0) {
                    queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C write/read requires TX bytes (register address)\",\"err\":-4}");
                    return "/ok.json";
                }
                int wret = i2c_write_timeout_us(PICOLAB_I2C0_ID, addr, buf, len, true, I2C_XFER_TIMEOUT_US);
                if (wret < 0) {
                    i2c0_recover_after_error();
                    queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C write/read write phase failed\",\"err\":%d}", wret);
                    return "/ok.json";
                }
            }
            
            int ret = i2c_read_timeout_us(PICOLAB_I2C0_ID, addr, buf, rlen, false, I2C_XFER_TIMEOUT_US);
            if (ret >= 0) {
                char hex_out[258] = {0};
                for(int i = 0; i < ret; i++) sprintf(hex_out + (i*2), "%02X", buf[i]);
                queue_bus_event_json("{\"bus\":\"i2c\",\"rx\":\"%s\",\"bytes\":%d}", hex_out, ret);
            } else {
                i2c0_recover_after_error();
                queue_bus_event_json("{\"bus\":\"i2c\",\"msg\":\"I2C read failed\",\"err\":%d}", ret);
            }
        }
    }

    return "/ok.json";
}

// HTTP CGI: SPI bus setup and transfers
const char * cgi_spi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *action = query_value(iNumParams, pcParam, pcValue, "action");
    if (!action) return "/ok.json";

    if (strcmp(action, "setup") == 0) {
        int baud = query_value_int(iNumParams, pcParam, pcValue, "baud", 1000000);
        spi0_apply_config((uint32_t)baud);
        queue_bus_event_json("{\"bus\":\"spi\",\"msg\":\"SPI setup at %d Hz on GPIO%d/GPIO%d/GPIO%d\"}", baud, PICOLAB_SPI_SCK_PIN, PICOLAB_SPI_MOSI_PIN, PICOLAB_SPI_MISO_PIN);
    }
    else {
        const char *data_str = query_value(iNumParams, pcParam, pcValue, "data");
        uint8_t buf[128];
        size_t len = 0;
        
        if (data_str) len = parse_hex_string(data_str, buf, sizeof(buf));
        spi_prepare_cs_pin(PICOLAB_SPI_DEFAULT_CS_PIN);

        if (strcmp(action, "write") == 0) {
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 0);
            int ret = spi_write_blocking(PICOLAB_SPI_ID, buf, len);
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 1);
            if (ret >= 0) {
                queue_bus_event_json("{\"bus\":\"spi\",\"msg\":\"SPI write ok\",\"bytes\":%d}", ret);
            } else {
                queue_bus_event_json("{\"bus\":\"spi\",\"msg\":\"SPI write failed\",\"err\":%d}", ret);
            }
        }
        else if (strcmp(action, "read") == 0) {
            int rlen = query_value_int(iNumParams, pcParam, pcValue, "rlen", 1);
            uint8_t send_val = query_value_int(iNumParams, pcParam, pcValue, "send", 0);
            if (rlen > sizeof(buf)) rlen = sizeof(buf);
            
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 0);
            int ret = spi_read_blocking(PICOLAB_SPI_ID, send_val, buf, rlen);
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 1);

            if (ret >= 0) {
                char hex_out[258] = {0};
                for(int i = 0; i < ret; i++) sprintf(hex_out + (i*2), "%02X", buf[i]);
                queue_bus_event_json("{\"bus\":\"spi\",\"rx\":\"%s\",\"bytes\":%d}", hex_out, ret);
            } else {
                queue_bus_event_json("{\"bus\":\"spi\",\"msg\":\"SPI read failed\",\"err\":%d}", ret);
            }
        }
        else if (strcmp(action, "writeread") == 0) {
            int rlen = query_value_int(iNumParams, pcParam, pcValue, "rlen", 1);
            uint8_t send_val = query_value_int(iNumParams, pcParam, pcValue, "send", 0);
            if (rlen > sizeof(buf)) rlen = sizeof(buf);
            
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 0);
            int wret = spi_write_blocking(PICOLAB_SPI_ID, buf, len);
            int rret = -1;
            
            // Only proceed to read if the write phase succeeded
            if (wret >= 0) {
                rret = spi_read_blocking(PICOLAB_SPI_ID, send_val, buf, rlen);
            }
            gpio_put(PICOLAB_SPI_DEFAULT_CS_PIN, 1);

            if (wret >= 0 && rret >= 0) {
                char hex_out[258] = {0};
                for(int i = 0; i < rret; i++) sprintf(hex_out + (i*2), "%02X", buf[i]);
                queue_bus_event_json("{\"bus\":\"spi\",\"rx\":\"%s\",\"bytes\":%d}", hex_out, rret);
            } else {
                queue_bus_event_json("{\"bus\":\"spi\",\"msg\":\"SPI writeread failed\",\"err\":%d}", (wret < 0) ? wret : rret);
            }
        }
    }
    return "/ok.json";
}

// Command Line Interface (CLI) over CDC USB Serial

// Outputs a raw string to the USB CDC interface
static void cli_puts_raw(const char *text) {
    if (!tud_cdc_connected() || !text) return;
    cli_write_all(text, strlen(text));
}

// Outputs a string followed by a carriage return and line feed
static void cli_puts_line(const char *text) {
    cli_puts_raw(text);
    cli_puts_raw("\r\n");
}

// Formats and outputs a string using printf-style arguments
static void cli_printf(const char *format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    cli_puts_raw(buf);
}

// Displays the standard CLI prompt indicator
static void cli_print_prompt(void) {
    cli_puts_raw("pico> ");
}

// Parses a string into an integer, automatically detecting hex (0x) or decimal
static bool parse_uint_auto(const char *text, unsigned int *value) {
    if (!text || !*text) return false;

    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (end && *end == '\0') {
        *value = (unsigned int)parsed;
        return true;
    }

    end = NULL;
    parsed = strtoul(text, &end, 16);
    if (end && *end == '\0') {
        *value = (unsigned int)parsed;
        return true;
    }

    return false;
}

// Converts a CLI pin token (e.g., "D2" or "2") into a valid managed pin index
static int cli_managed_pin_index(const char *token) {
    if (!token || !token[0]) return -1;
    if (token[0] == 'D' || token[0] == 'd') {
        int idx = atoi(token + 1) - 1;
        return is_valid_pin_index(idx) ? idx : -1;
    }
    int idx = atoi(token) - 1;
    return is_valid_pin_index(idx) ? idx : -1;
}

// Converts a CLI ADC token (e.g., "A1" or "1") into a valid analog channel index
static int cli_adc_channel_index(const char *token) {
    if (!token || !token[0]) return -1;
    if (token[0] == 'A' || token[0] == 'a') {
        int idx = atoi(token + 1) - 1;
        return (idx >= 0 && idx < NUM_ANALOG_CHANNELS) ? idx : -1;
    }
    int idx = atoi(token) - 1;
    return (idx >= 0 && idx < NUM_ANALOG_CHANNELS) ? idx : -1;
}

// Converts raw binary bytes into a formatted uppercase hexadecimal string
static void cli_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_len) {
    if (!out_len) return;
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
        int written = snprintf(out + pos, out_len - pos, "%02X", data[i]);
        if (written <= 0) break;
        pos += (size_t)written;
    }
    if (pos < out_len) out[pos] = '\0';
}

// Blocks and pushes data to the USB CDC interface until fully transmitted
static void cli_write_all(const char *data, size_t len) {
    if (!tud_cdc_connected() || !data || len == 0) return;

    size_t sent = 0;
    while (sent < len && tud_cdc_connected()) {
        uint32_t written = tud_cdc_write(data + sent, len - sent);
        if (written > 0) {
            sent += written;
            continue;
        }
        tud_task();
        tud_cdc_write_flush();
    }
    tud_cdc_write_flush();
}

static void cli_print_help(void) {
    cli_puts_line("Commands:");
    cli_puts_line("  help");
    cli_puts_line("  gpio status");
    cli_puts_line("  gpio read Dn");
    cli_puts_line("  gpio write Dn 0|1");
    cli_puts_line("  gpio mode Dn in|out|pwm");
    cli_puts_line("  gpio pull Dn up|hi-z");
    cli_puts_line("  gpio drive Dn 2|4|8|12");
    cli_puts_line("  gpio pwm Dn duty <0-100>");
    cli_puts_line("  gpio pwm Dn freq <hz>");
    cli_puts_line("  adc read A<n>");
    cli_puts_line("  adc all");
    cli_puts_line("  power status");
    cli_puts_line("  power on | off");
    cli_puts_line("  power 5v on | off");
    cli_puts_line("  power 3v3 on | off");
    cli_puts_line("  power adj on | off");
    cli_puts_line("  power adj voltage <0-12>");
    cli_puts_line("  power adj current <mA>");
    cli_puts_line("  i2c setup [baud]");
    cli_puts_line("  i2c scan");
    cli_puts_line("  i2c write <addr> <hex>");
    cli_puts_line("  i2c read <addr> [rlen]");
    cli_puts_line("  i2c wr <addr> <hex> [rlen]");
    cli_puts_line("  spi setup [baud]");
    cli_puts_line("  spi write <hex> [cs=5]");
    cli_puts_line("  spi read [rlen] [send=00] [cs=5]");
    cli_puts_line("  sys info | reboot | bootsel");
}

// Queries hardware registers and linker boundaries to report system health/stats
static void cli_print_sysinfo(void) {
#ifndef SYSINFO_BASE
#define SYSINFO_BASE 0x40000000
#endif
    uint32_t chip_id = *(volatile uint32_t *)(SYSINFO_BASE);
    uint8_t chip_rev = (chip_id >> 28) & 0xF;
    const char* chip_name = "Unknown";
    uint32_t flash_used_bytes = (uint32_t)(&__flash_binary_end - &__flash_binary_start);
    float flash_used_mb = (float)flash_used_bytes / (1024.0f * 1024.0f);

#if defined(PICO_RP2350)
    uint32_t pkg = *(volatile uint32_t *)(SYSINFO_BASE + 4);
    chip_name = ((pkg & 1) == 0) ? "RP2350B" : "RP2350A";
#elif defined(PICO_RP2040)
    chip_name = "RP2040";
#else
    chip_name = "RP-Unknown";
#endif

    cli_printf("FW: Pico.Lab v%s\r\n", PICOLAB_FIRMWARE_VERSION);
    cli_printf("Board: %s\r\n", PICOLAB_BOARD_NAME);
    cli_printf("Chip: %s (rev %u)\r\n", chip_name, chip_rev);
    cli_printf("Serial: %s\r\n", global_serial_str);
    cli_printf("Flash: %lu MB (%.2f MB used)\r\n", global_flash_mb, flash_used_mb);
    cli_printf("Clock: %lu MHz\r\n", clock_get_hz(clk_sys) / 1000000);
    cli_printf("ADC scale: 0..%.1f V (%d ch)\r\n", PICOLAB_ADC_FULL_SCALE_V, PICOLAB_ADC_ACTIVE_CHANNELS);
}

// Retrieves and formats power rail telemetry
static void cli_print_power_status(void) {
    if (!PICOLAB_HAS_ADJ_RAIL) {
        cli_puts_line("Power control is not available on this board.");
        return;
    }

    psu_refresh_telemetry();
    cli_printf("Power rails: 5V=%s 3V3=%s ADJ=%s\r\n",
               psu_5v_enabled ? "on" : "off",
               psu_3v3_enabled ? "on" : "off",
               psu_adj_enabled ? "on" : "off");
    cli_printf("ADJ setpoint: %.2f V, %u mA\r\n",
               psu_adj_voltage_mv / 1000.0f,
               psu_adj_current_ma);
    cli_printf("Telemetry: 5V=%u mA 3V3=%u mA ADJ=%u mA @ %.2f V\r\n",
               shared_psu_mock[0], shared_psu_mock[1], shared_psu_mock[2],
               shared_psu_mock[3] / 1000.0f);
}

// CLI handler for 'power' commands
static void cli_cmd_power(int argc, char *argv[]) {
    if (!PICOLAB_HAS_ADJ_RAIL) {
        cli_puts_line("ERR: power control is only available on custom hardware.");
        return;
    }

    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        cli_print_power_status();
        return;
    }

    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        psu_set_rail_state("all", strcmp(argv[1], "on") == 0);
        cli_print_power_status();
        return;
    }

    if (strcmp(argv[1], "5v") == 0 || strcmp(argv[1], "3v3") == 0 || strcmp(argv[1], "adj") == 0) {
        if (argc < 3) {
            cli_puts_line("ERR: power rail needs on/off, or adj needs voltage/current.");
            return;
        }

        if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
            psu_set_rail_state(argv[1], strcmp(argv[2], "on") == 0);
            cli_print_power_status();
            return;
        }

        if (strcmp(argv[1], "adj") == 0 && strcmp(argv[2], "voltage") == 0) {
            if (argc < 4) {
                cli_puts_line("ERR: power adj voltage needs a value.");
                return;
            }
            psu_set_adj_voltage((float)atof(argv[3]));
            cli_print_power_status();
            return;
        }

        if (strcmp(argv[1], "adj") == 0 && strcmp(argv[2], "current") == 0) {
            if (argc < 4) {
                cli_puts_line("ERR: power adj current needs a value.");
                return;
            }
            unsigned int limit = 0;
            if (!parse_uint_auto(argv[3], &limit)) {
                cli_puts_line("ERR: invalid current value.");
                return;
            }
            psu_set_adj_current(limit);
            cli_print_power_status();
            return;
        }
    }

    cli_puts_line("ERR: unknown power command.");
}

// CLI handler for 'gpio' commands
static void cli_cmd_gpio(int argc, char *argv[]) {
    if (argc < 2) {
        cli_print_help();
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        for (int i = 0; i < NUM_MANAGED_PINS; i++) {
            uint8_t phys_pin = MANAGED_PINS[i];
            cli_printf("D%d (GPIO%u) = %u\r\n", i+1, phys_pin, gpio_get(phys_pin) ? 1u : 0u);
        }
        return;
    }

    if (argc < 3) {
        cli_puts_line("ERR: missing pin. Example: gpio read D2");
        return;
    }

    int pin_idx = cli_managed_pin_index(argv[2]);
    if (!is_valid_pin_index(pin_idx)) {
        cli_puts_line("ERR: invalid managed pin. Use D1-D8.");
        return;
    }

    uint8_t phys_pin = MANAGED_PINS[pin_idx];

    if (strcmp(argv[1], "read") == 0) {
        cli_printf("D%d = %s\r\n", pin_idx+1, gpio_get(phys_pin) ? "HIGH" : "LOW");
    } else if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            cli_puts_line("ERR: gpio write needs a value (0 or 1).");
            return;
        }
        board_set_pin_direction(pin_idx, true);
        gpio_put(phys_pin, atoi(argv[3]) ? 1 : 0);
        cli_printf("D%d <- %s\r\n", pin_idx+1, gpio_get(phys_pin) ? "HIGH" : "LOW");
    } else if (strcmp(argv[1], "mode") == 0) {
        if (argc < 4) {
            cli_puts_line("ERR: gpio mode needs in, out, or pwm.");
            return;
        }
        config_pin_mode(pin_idx, phys_pin, argv[3]);
        cli_printf("D%d mode set to %s\r\n", pin_idx+1, argv[3]);
    } else if (strcmp(argv[1], "pull") == 0) {
        if (argc < 4) {
            cli_puts_line("ERR: gpio pull needs up or hi-z.");
            return;
        }
        bool pull_up = strcmp(argv[3], "up") == 0;
        if (strcmp(argv[3], "up") != 0 && strcmp(argv[3], "hi-z") != 0) {
            cli_puts_line("ERR: gpio pull needs up or hi-z.");
            return;
        }
        config_pin_pull(pin_idx, phys_pin, pull_up);
        cli_printf("D%d pull set to %s\r\n", pin_idx+1, argv[3]);
    } else if (strcmp(argv[1], "drive") == 0) {
        if (argc < 4) {
            cli_puts_line("ERR: gpio drive needs 2, 4, 8, or 12.");
            return;
        }
        int drive = atoi(argv[3]);
        if (!set_gpio_drive_strength(phys_pin, drive)) {
            cli_puts_line("ERR: drive must be 2, 4, 8, or 12.");
            return;
        }
        cli_printf("D%d drive set to %d mA\r\n", pin_idx+1, drive);
    } else if (strcmp(argv[1], "pwm") == 0) {
        if (argc < 5) {
            cli_puts_line("ERR: gpio pwm needs duty or freq plus a value.");
            return;
        }
        config_pin_mode(pin_idx, phys_pin, "pwm");
        uint slice_num = pwm_gpio_to_slice_num(phys_pin);
        if (strcmp(argv[3], "duty") == 0) {
            int duty = atoi(argv[4]);
            if (duty < 0) duty = 0;
            if (duty > 100) duty = 100;
            pwm_set_chan_level(slice_num, pwm_gpio_to_channel(phys_pin), duty * 100);
            cli_printf("D%d PWM duty = %d%%\r\n", pin_idx+1, duty);
        } else if (strcmp(argv[3], "freq") == 0) {
            unsigned int freq = 0;
            if (!parse_uint_auto(argv[4], &freq) || freq == 0) {
                cli_puts_line("ERR: invalid PWM frequency.");
                return;
            }
            float div = (float)clock_get_hz(clk_sys) / (freq * 10000.0f);
            if (div < 1.0f) div = 1.0f;
            if (div > 255.9f) div = 255.9f;
            pwm_set_clkdiv(slice_num, div);
            cli_printf("D%d PWM frequency = %u Hz\r\n", pin_idx+1, freq);
        } else {
            cli_puts_line("ERR: gpio pwm needs duty or freq.");
        }
    } else {
        cli_puts_line("ERR: unknown gpio command.");
    }
}

// CLI handler for 'adc' commands
static void cli_cmd_adc(int argc, char *argv[]) {
    if (argc < 2) {
        cli_puts_line("ERR: adc needs read or all.");
        return;
    }

    if (strcmp(argv[1], "all") == 0) {
        for (int i = 0; i < NUM_ANALOG_CHANNELS; i++) {
            if (!is_analog_channel_available(i)) continue;
            float volts = analog_raw_to_volts(shared_analog[i]);
            cli_printf("A%d = %u raw, %.3f V\r\n", i+1, shared_analog[i], volts);
        }
        return;
    }

    if (strcmp(argv[1], "read") == 0 && argc >= 3) {
        int ch = cli_adc_channel_index(argv[2]);
        if (ch < 0 || !is_analog_channel_available(ch)) {
            cli_puts_line("ERR: invalid ADC channel for this board.");
            return;
        }
        float volts = analog_raw_to_volts(shared_analog[ch]);
        cli_printf("A%d = %u raw, %.3f V\r\n", ch+1, shared_analog[ch], volts);
        return;
    }

    cli_puts_line("ERR: adc command not understood.");
}

// CLI handler for 'sys' commands
static void cli_cmd_sys(int argc, char *argv[]) {
    if (argc < 2) {
        cli_print_help();
        return;
    }

    if (strcmp(argv[1], "info") == 0) {
        cli_print_sysinfo();
    } else if (queue_system_command(argv[1])) {
        if (strcmp(argv[1], "bootsel") == 0) cli_puts_line("BOOTSEL reboot scheduled.");
        else cli_puts_line("Reboot scheduled.");
    } else {
        cli_puts_line("ERR: unknown sys command.");
    }
}

// CLI handler for 'i2c' commands
static void cli_cmd_i2c(int argc, char *argv[]) {
    if (argc < 2) {
        cli_puts_line("ERR: i2c needs setup, scan, read, write, or wr.");
        return;
    }

    if (strcmp(argv[1], "setup") == 0) {
        unsigned int baud = 100000;
        if (argc >= 3) parse_uint_auto(argv[2], &baud);
        i2c0_apply_config(baud);
        cli_printf("I2C setup at %u Hz on GPIO%d/GPIO%d\r\n", baud, PICOLAB_I2C0_SDA_PIN, PICOLAB_I2C0_SCL_PIN);
        return;
    }

    if (strcmp(argv[1], "scan") == 0) {
        if (!i2c0_configured) i2c0_apply_config(i2c0_baud_hz);

        char addrs[256] = "";
        int count = 0;
        for (int addr = 0x08; addr < 0x78; ++addr) {
            uint8_t rxdata;
            int sret = i2c_read_timeout_us(PICOLAB_I2C0_ID, addr, &rxdata, 1, false, I2C_SCAN_TIMEOUT_US);
            if (sret >= 0) {
                char entry[8];
                snprintf(entry, sizeof(entry), "0x%02X ", addr);
                strncat(addrs, entry, sizeof(addrs) - strlen(addrs) - 1);
                count++;
            }
        }
        if (count == 0) cli_puts_line("I2C scan: no devices found.");
        else cli_printf("I2C scan: %d device%s: %s\r\n", count, (count == 1) ? "" : "s", addrs);
        return;
    }

    if (argc < 4) {
        cli_puts_line("ERR: i2c read/write needs address and data/rlen.");
        return;
    }

    if (!i2c0_configured) i2c0_apply_config(i2c0_baud_hz);

    unsigned int addr = 0;
    if (!parse_uint_auto(argv[2], &addr) || addr < 0x08 || addr > 0x77) {
        cli_puts_line("ERR: invalid I2C address.");
        return;
    }

    uint8_t buf[128];
    size_t len = 0;

    if (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "wr") == 0) {
        if (!parse_hex_string_strict(argv[3], buf, sizeof(buf), &len)) {
            cli_puts_line("ERR: invalid hex payload.");
            return;
        }
    }

    if (strcmp(argv[1], "write") == 0) {
        int ret = i2c_write_timeout_us(PICOLAB_I2C0_ID, addr, buf, len, false, I2C_XFER_TIMEOUT_US);
        if (ret >= 0) cli_printf("I2C write ok (%d bytes)\r\n", ret);
        else cli_printf("I2C write failed (%d)\r\n", ret);
        return;
    }

    int rlen = 1;
    if (strcmp(argv[1], "read") == 0) {
        if (argc >= 4) parse_uint_auto(argv[3], (unsigned int *)&rlen);
    } else {
        if (argc >= 5) parse_uint_auto(argv[4], (unsigned int *)&rlen);
    }
    if (rlen <= 0) rlen = 1;
    if (rlen > (int)sizeof(buf)) rlen = sizeof(buf);

    if (strcmp(argv[1], "wr") == 0) {
        int wret = i2c_write_timeout_us(PICOLAB_I2C0_ID, addr, buf, len, true, I2C_XFER_TIMEOUT_US);
        if (wret < 0) {
            i2c0_recover_after_error();
            cli_printf("I2C write/read write phase failed (%d)\r\n", wret);
            return;
        }
    }

    int ret = i2c_read_timeout_us(PICOLAB_I2C0_ID, addr, buf, rlen, false, I2C_XFER_TIMEOUT_US);
    if (ret >= 0) {
        char hex_out[258] = {0};
        cli_bytes_to_hex(buf, (size_t)ret, hex_out, sizeof(hex_out));
        cli_printf("I2C 0x%02X -> %s\r\n", (unsigned int)addr, hex_out);
    } else {
        i2c0_recover_after_error();
        cli_printf("I2C read failed (%d)\r\n", ret);
    }
}

// CLI handler for 'spi' commands
static void cli_cmd_spi(int argc, char *argv[]) {
    if (argc < 2) {
        cli_puts_line("ERR: spi needs setup, write, or read.");
        return;
    }

    if (strcmp(argv[1], "setup") == 0) {
        unsigned int baud = 1000000;
        if (argc >= 3) parse_uint_auto(argv[2], &baud);
        spi0_apply_config(baud);
        cli_printf("SPI setup at %u Hz on GPIO%d/GPIO%d/GPIO%d\r\n", baud, PICOLAB_SPI_SCK_PIN, PICOLAB_SPI_MOSI_PIN, PICOLAB_SPI_MISO_PIN);
        return;
    }

    unsigned int cs_pin = PICOLAB_SPI_DEFAULT_CS_PIN;
    unsigned int send_val = 0;
    unsigned int rlen = 1;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "cs=", 3) == 0) parse_uint_auto(argv[i] + 3, &cs_pin);
        else if (strncmp(argv[i], "send=", 5) == 0) parse_uint_auto(argv[i] + 5, &send_val);
    }

    if (strcmp(argv[1], "write") == 0) {
        if (argc < 3) {
            cli_puts_line("ERR: spi write needs hex data.");
            return;
        }
        uint8_t buf[128];
        size_t len = 0;
        if (!parse_hex_string_strict(argv[2], buf, sizeof(buf), &len)) {
            cli_puts_line("ERR: invalid hex payload.");
            return;
        }
        spi_prepare_cs_pin(cs_pin);
        gpio_put(cs_pin, 0);
        int ret = spi_write_blocking(PICOLAB_SPI_ID, buf, len);
        gpio_put(cs_pin, 1);
        cli_printf("SPI write %s (%d bytes)\r\n", (ret >= 0) ? "ok" : "failed", ret);
        return;
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc >= 3) parse_uint_auto(argv[2], &rlen);
        if (rlen == 0) rlen = 1;
        if (rlen > 128) rlen = 128;

        uint8_t buf[128];
        spi_prepare_cs_pin(cs_pin);
        gpio_put(cs_pin, 0);
        int ret = spi_read_blocking(PICOLAB_SPI_ID, (uint8_t)send_val, buf, rlen);
        gpio_put(cs_pin, 1);
        if (ret >= 0) {
            char hex_out[258] = {0};
            cli_bytes_to_hex(buf, (size_t)ret, hex_out, sizeof(hex_out));
            cli_printf("SPI -> %s\r\n", hex_out);
        } else {
            cli_printf("SPI read failed (%d)\r\n", ret);
        }
        return;
    }

    cli_puts_line("ERR: unknown spi command.");
}

// Take user input and route it to the corresponding CLI command
static void cli_execute_line(char *line) {
    char *argv[12] = {0};
    int argc = 0;

    for (char *tok = strtok(line, " \t\r\n"); tok && argc < 12; tok = strtok(NULL, " \t\r\n")) {
        argv[argc++] = tok;
    }

    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) cli_print_help();
    else if (strcmp(argv[0], "gpio") == 0) cli_cmd_gpio(argc, argv);
    else if (strcmp(argv[0], "adc") == 0) cli_cmd_adc(argc, argv);
    else if (strcmp(argv[0], "power") == 0) cli_cmd_power(argc, argv);
    else if (strcmp(argv[0], "sys") == 0 || strcmp(argv[0], "system") == 0) cli_cmd_sys(argc, argv);
    else if (strcmp(argv[0], "i2c") == 0) cli_cmd_i2c(argc, argv);
    else if (strcmp(argv[0], "spi") == 0) cli_cmd_spi(argc, argv);
    else cli_puts_line("ERR: unknown command. Type 'help'.");
}

// Core task to handle receiving keys and rendering the CLI shell over USB
static void cli_usb_task(void) {
    static bool banner_sent = false;

    if (!tud_cdc_connected()) {
        cli_rx_len = 0;
        banner_sent = false;
        return;
    }

    if (!banner_sent) {
        cli_puts_line("PicoLab CLI ready. Type 'help'.");
        cli_print_prompt();
        banner_sent = true;
    }

    while (tud_cdc_available()) {
        uint8_t ch = 0;
        uint32_t count = tud_cdc_read(&ch, 1);
        if (count == 0) break;

        // Process Enter key to submit command
        if (ch == '\r' || ch == '\n') {
            cli_puts_raw("\r\n");
            if (cli_rx_len > 0) {
                cli_rx_buffer[cli_rx_len] = '\0';
                cli_execute_line(cli_rx_buffer);
                cli_rx_len = 0;
                cli_print_prompt();
            }
            continue;
        }

        // Process Backspace key
        if (ch == 0x08 || ch == 0x7F) {
            if (cli_rx_len > 0) {
                cli_rx_len--;
                cli_puts_raw("\b \b");
            }
            continue;
        }

        // Store standard characters to command buffer
        if (cli_rx_len < CLI_RX_BUF_SIZE - 1) {
            cli_rx_buffer[cli_rx_len++] = (char)ch;
            cli_write_all((const char *)&ch, 1);
        } else {
            cli_puts_line("ERR: line too long.");
            cli_rx_len = 0;
            cli_print_prompt();
        }
    }
}

// Configures hardware UART for routing to the web dashboard
static void hw_uart_bridge_init(void) {
    uart_init(PICOLAB_UART_ID, uart_baud_hz);
    uart_set_format(PICOLAB_UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(PICOLAB_UART_ID, true);
    gpio_set_function(PICOLAB_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(PICOLAB_UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(PICOLAB_UART_TX_PIN);
    gpio_pull_up(PICOLAB_UART_RX_PIN);
}

// Drains hardware UART RX FIFO into memory buffer
static void hw_uart_rx_task(void) {
    while (uart_is_readable(PICOLAB_UART_ID) && uart_rx_len < UART_RX_BUF_SIZE - 1) {
        uart_rx_buffer[uart_rx_len++] = (char)uart_getc(PICOLAB_UART_ID);
        has_new_uart_data = true;
    }
}

// Writes pending buffer data out over hardware UART TX
static void hw_uart_tx_task(void) {
    while (uart_tx_tail != uart_tx_head && uart_is_writable(PICOLAB_UART_ID)) {
        uart_putc_raw(PICOLAB_UART_ID, uart_tx_buffer[uart_tx_tail]);
        uart_tx_tail = (uart_tx_tail + 1) % UART_TX_BUF_SIZE;
    }
}

// HTTP CGI routing map for internal web API
static const tCGI cgi_handlers[] = {
    {"/toggle", cgi_toggle_handler},
    {"/config", cgi_config_handler},
    {"/system", cgi_system_handler},
    {"/uart", cgi_uart_handler},
    {"/power", cgi_power_handler},
    {"/scope", cgi_scope_handler},
    {"/logic", cgi_logic_handler},
    {"/i2c", cgi_i2c_handler},
    {"/spi", cgi_spi_handler}
};

// Server-Sent Events (SSE) streaming engine for pushing real-time scope/telemetry data

// Resets internal state tracking when TCP SSE connection fails or closes
static void sse_err_cb(void *arg, err_t err) {
    (void) arg;
    (void) err;
    sse_pcb = NULL; 
    sse_headers_sent = false;
    browser_connection_active = false;
}

// Handles incoming client packets on SSE connection port
static err_t sse_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void) arg;
    (void) err;
    if (p != NULL) {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        
        // Transmit required HTTP headers exactly once upon new connection
        if (!sse_headers_sent) {
            const char *sse_headers = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n";
            tcp_write(tpcb, sse_headers, strlen(sse_headers), TCP_WRITE_FLAG_COPY);
            tcp_output(tpcb);
            sse_headers_sent = true; 
            browser_connection_active = true;
            send_sysinfo_pending = true;
        }
    } else {
        // Client initiated disconnect via FIN packet
        tcp_close(tpcb);
        sse_pcb = NULL;
        sse_headers_sent = false;
        browser_connection_active = false;
    }
    return ERR_OK;
}

// Callback fired by lwIP when a new TCP client connects to SSE server
static err_t sse_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void) arg;
    (void) err;
    if (sse_pcb != NULL) {
        tcp_abort(sse_pcb);
    }
    sse_pcb = newpcb; 
    sse_headers_sent = false; 
    browser_connection_active = false;
    tcp_nagle_disable(newpcb); 
    tcp_err(newpcb, sse_err_cb);
    tcp_recv(newpcb, sse_recv_cb);
    return ERR_OK;
}

// Starts the TCP listener for the live telemetry datastream
void sse_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    tcp_bind(pcb, IP_ADDR_ANY, 8080);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, sse_accept_cb);
}

// Pushes buffered incoming hardware UART bytes up to browser UI
static bool handle_sse_uart(void) {
    if (!has_new_uart_data) return false;

    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "event: uart\ndata: {\"text\": \"");
    
    static const char hex_digits[] = "0123456789abcdef";
    int chars_to_process = 0;
    int max_chars = (uart_rx_len > 128) ? 128 : uart_rx_len;
    for (int i = 0; i < max_chars; i++) {
        unsigned char c = (unsigned char)uart_rx_buffer[i];
        int escaped_len = (c < 0x20 && c != '\n' && c != '\r') ? 6 :
                          (c == '\n' || c == '\r' || c == '"' || c == '\\') ? 2 : 1;
        if (pos + escaped_len + 5 > (int)sizeof(buf)) break;

        if (c == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
        else if (c == '\r') { buf[pos++] = '\\'; buf[pos++] = 'r'; }
        else if (c == '"') { buf[pos++] = '\\'; buf[pos++] = '"'; }
        else if (c == '\\') { buf[pos++] = '\\'; buf[pos++] = '\\'; }
        else if (c < 0x20) {
            buf[pos++] = '\\'; buf[pos++] = 'u';
            buf[pos++] = '0'; buf[pos++] = '0';
            buf[pos++] = hex_digits[c >> 4];
            buf[pos++] = hex_digits[c & 0x0F];
        } else { buf[pos++] = (char)c; }
        chars_to_process++;
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\"}\n\n");

    if (tcp_sndbuf(sse_pcb) > strlen(buf)) {
        err_t err = tcp_write(sse_pcb, buf, strlen(buf), TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK) {
            int remaining = uart_rx_len - chars_to_process;
            if (remaining > 0) {
                memmove(uart_rx_buffer, uart_rx_buffer + chars_to_process, (size_t)remaining);
                uart_rx_len = remaining;
            } else {
                has_new_uart_data = false;
                uart_rx_len = 0;
            }
            return true;
        }
    }
    return false;
}

// Sends complete system health/stats JSON over SSE event stream
static bool handle_sse_sysinfo(void) {
    if (!send_sysinfo_pending) return false;

#ifndef SYSINFO_BASE
#define SYSINFO_BASE 0x40000000
#endif
    uint32_t chip_id = *(volatile uint32_t *)(SYSINFO_BASE);
    uint8_t chip_rev = (chip_id >> 28) & 0xF;
    const char* chip_name = "Unknown";
    uint32_t flash_used_bytes = (uint32_t)(&__flash_binary_end - &__flash_binary_start);
    float flash_used_mb = (float)flash_used_bytes / (1024.0f * 1024.0f);

    // Call our new system RAM functions
    uint32_t total_ram_kb = get_total_ram() / 1024;
    float used_ram_kb = (float)get_used_ram() / 1024.0f;
    
#if defined(PICO_RP2350)
    uint32_t pkg = *(volatile uint32_t *)(SYSINFO_BASE + 4);
    chip_name = ((pkg & 1) == 0) ? "RP2350B" : "RP2350A";
#elif defined(PICO_RP2040)
    chip_name = "RP2040";
#else
    chip_name = "RP-Unknown";
#endif

    char buf[256];
    int len = snprintf(buf, sizeof(buf), 
        "event: sysinfo\n"
        "data: {\"fw\":\"Version %s\",\"board\":\"%s\",\"chip\":\"%s\",\"rev\":%d,\"serial\":\"%s\",\"flash\":%lu,\"flash_used\":%.2f,\"ram\":%lu,\"ram_used\":%.f,\"clk\":%lu,\"adc_fs\":%.1f,\"adc_ch\":%d,\"has_adj\":%d,\"ip\":\"%s\"}\n\n",
        PICOLAB_FIRMWARE_VERSION, PICOLAB_BOARD_NAME, chip_name, chip_rev, global_serial_str,
        global_flash_mb, flash_used_mb, total_ram_kb, used_ram_kb, clock_get_hz(clk_sys) / 1000000,
        PICOLAB_ADC_FULL_SCALE_V, PICOLAB_ADC_ACTIVE_CHANNELS, PICOLAB_HAS_ADJ_RAIL ? 1 : 0,
        ip4addr_ntoa(&ipaddr));

    if (tcp_sndbuf(sse_pcb) > (uint16_t)len) {
        tcp_write(sse_pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
        send_sysinfo_pending = false;
        return true;
    }
    return false;
}

// Transmits real-time GPIO and PSU states to sync the web UI indicators
static bool handle_sse_gpio(void) {
    psu_refresh_telemetry();
    char buf[128];
    int pos = snprintf(buf, sizeof(buf), "data: %04X%04X%04X%04X%04X%04X%04X%04X%04X\n\n", 
                    shared_analog[0], shared_analog[1], shared_analog[2], shared_analog[3],
                    shared_psu_mock[0], shared_psu_mock[1], shared_psu_mock[2], shared_psu_mock[3],
                    shared_digital);

    if (tcp_sndbuf(sse_pcb) > (uint16_t)pos) {
        tcp_write(sse_pcb, buf, (u16_t)pos, TCP_WRITE_FLAG_COPY);
        return true;
    }
    return false;
}

// Formats binary oscilloscope capture data into Base64 and pushes via SSE
static bool handle_sse_scope(void) {
    if (!scope_data_ready) return false;

    static char b64_buf[((MAX_SCOPE_SAMPLES * 2) * 4 / 3) + 4]; 
    static char sse_out_buf[((MAX_SCOPE_SAMPLES * 2) * 4 / 3) + 100]; 

    base64_encode((uint8_t*)scope_buffer, (size_t)(scope_current_samples * 2), b64_buf);

    int total_len = snprintf(sse_out_buf, sizeof(sse_out_buf), 
        "event: scope\ndata: {\"ch\":%d,\"tb\":%.1f,\"mask\":%d,\"data\":\"%s\"}\n\n", 
        scope_trig_ch, (double)scope_timebase_ms, scope_active_mask, b64_buf);

    if (tcp_sndbuf(sse_pcb) >= (uint16_t)total_len) {
        err_t err = tcp_write(sse_pcb, sse_out_buf, (u16_t)total_len, TCP_WRITE_FLAG_COPY);

        if (err == ERR_OK) {
            scope_data_ready = false; 
            if (scope_mode == 0 || scope_mode == 2) {
                scope_triggered = false; 
            } else if (scope_mode == 1) {
                scope_running = false; 
            }
            return true;
        } else if (err == ERR_MEM) {
            return false; 
        } else {
            scope_data_ready = false;
            scope_triggered = false;
            hunt_index = CAPTURE_BUF_SIZE;
        }
    }
    return false;
}

// Queues async I2C/SPI transactions results back to the browser
static bool handle_sse_bus(void) {
    if (!bus_event_pending) return false;

    char evt_buf[640];
    int len = snprintf(evt_buf, sizeof(evt_buf), "event: bus\ndata: %s\n\n", bus_event_payload);
    if (len <= 0 || len >= (int)sizeof(evt_buf)) {
        bus_event_pending = false;
        return false;
    }

    if (tcp_sndbuf(sse_pcb) >= (uint16_t)len) {
        if (tcp_write(sse_pcb, evt_buf, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            bus_event_pending = false;
            return true;
        }
    }
    return false;
}

// Packs long logical channel sample series into Run-Length Encoding (RLE) byte pairs [value, count]
// to maximize transmission efficiency across netif buffers.
size_t rle_compress_logic(volatile uint8_t* raw_data, size_t raw_len, uint8_t* out_buf, size_t max_out) {
    size_t out_idx = 0;
    uint8_t current_val = raw_data[0];
    uint8_t current_count = 1;

    for (size_t i = 1; i < raw_len; i++) {
        if (raw_data[i] == current_val && current_count < 255) {
            current_count++;
        } else {
            if (out_idx >= max_out - 2) break; 
            out_buf[out_idx++] = current_val;
            out_buf[out_idx++] = current_count;
            current_val = raw_data[i];
            current_count = 1;
        }
    }
    if (out_idx < max_out - 2) {
        out_buf[out_idx++] = current_val;
        out_buf[out_idx++] = current_count;
    }
    return out_idx;
}

// Processes the large logic analyzer buffer over SSE by chunking it to avoid memory exhaustion
static bool handle_sse_logic(void) {
    static int send_offset = 0;
    static int send_state = 0; 
    static uint32_t last_chunk_time = 0;
    static uint32_t send_generation = 0;
    static char b64_buf[516]; 
    static char sse_buf[600]; 
    
    static uint8_t rle_buf[MAX_LOGIC_SAMPLES]; 
    static size_t rle_len = 0;

    if (!logic_data_ready) {
        send_state = 0;
        return false;
    }

    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_chunk_time < 5) {
        return false;
    }

    if (send_state == 0) {
        rle_len = rle_compress_logic(logic_buffer, MAX_LOGIC_SAMPLES, rle_buf, sizeof(rle_buf));
        send_generation = logic_generation;

        const char* start_evt = "event: logic_start\ndata: {}\n\n";
        if (tcp_sndbuf(sse_pcb) >= strlen(start_evt)) {
            if (tcp_write(sse_pcb, start_evt, strlen(start_evt), TCP_WRITE_FLAG_COPY) == ERR_OK) {
                send_state = 1;
                send_offset = 0;
                last_chunk_time = now;
                return true; 
            }
        }
        return false;
    }

    if (send_state == 1) {
        int raw_chunk_size = 384; 
        if ((size_t)(rle_len - (size_t)send_offset) < (size_t)raw_chunk_size) {
            raw_chunk_size = (int)(rle_len - (size_t)send_offset);
        }

        int b64_chunk_size = ((raw_chunk_size + 2) / 3) * 4;

        if (tcp_sndbuf(sse_pcb) >= (uint16_t)(b64_chunk_size + 30)) {
            base64_encode(&rle_buf[send_offset], (size_t)raw_chunk_size, b64_buf);
            int len = snprintf(sse_buf, sizeof(sse_buf), "event: logic_data\ndata: %s\n\n", b64_buf);
            
            if (tcp_write(sse_pcb, sse_buf, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK) {
                send_offset += raw_chunk_size;
                last_chunk_time = now;

                if ((size_t)send_offset >= rle_len) {
                    send_state = 2;
                }
                return true; 
            }
        }
        return false; 
    }

    if (send_state == 2) {
        char end_evt[128];
        int len = snprintf(end_evt, sizeof(end_evt), "event: logic_end\ndata: {\"rate\":%.0f,\"generation\":%u}\n\n", (double)logic_sample_rate_hz, send_generation);
        if (tcp_sndbuf(sse_pcb) >= (uint16_t)len) {
            if (tcp_write(sse_pcb, end_evt, (u16_t)len, TCP_WRITE_FLAG_COPY) == ERR_OK) { 
                send_state = 0;
                logic_data_ready = false; 
                last_chunk_time = now;
                return true;
            }
        }
    }

    return false;
}

// Telemetry poll loop for organizing SSE data streams
void sse_telemetry_task(void) {
    static uint32_t last_gpio_time = 0;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    hw_uart_rx_task();

    if (sse_pcb != NULL && sse_headers_sent) {
        bool output_needed = false;

        // Pause non-critical updates during heavy logic or scope streams to avoid buffer starves
        if (!logic_data_ready && !scope_data_ready) {
            output_needed |= handle_sse_uart();
            output_needed |= handle_sse_sysinfo();
            output_needed |= handle_sse_bus();

            if (now - last_gpio_time >= 10) {
                last_gpio_time = now;
                output_needed |= handle_sse_gpio();
            }
        }

        output_needed |= handle_sse_scope();
        output_needed |= handle_sse_logic();

        if (output_needed) tcp_output(sse_pcb);
    }
}

// HTTP POST handlers for sending data out over physical UART
void uart_tx_task(void) {
    hw_uart_tx_task();
}

// Intercepts and configures HTTP POST request aimed at the UART interface
err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                       u16_t http_request_len, int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd) {
    (void) http_request;
    (void) http_request_len;
    (void) content_len;
    (void) post_auto_wnd;
    if (strcmp(uri, "/uart_tx") == 0) {
        current_post_connection = connection;
        snprintf(response_uri, response_uri_len, "/index.html"); 
        return ERR_OK;
    }
    return ERR_VAL; 
}

// Copies chunks of the HTTP POST body directly into the UART transmit ring buffer
err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    if (connection == current_post_connection && p != NULL) {
        for (uint16_t i = 0; i < p->len; i++) {
            uint16_t next_head = (uart_tx_head + 1) % UART_TX_BUF_SIZE;
            if (next_head != uart_tx_tail) { 
                uart_tx_buffer[uart_tx_head] = ((uint8_t*)p->payload)[i];
                uart_tx_head = next_head;
            } else break; 
        }
        pbuf_free(p); 
        return ERR_OK;
    }
    if (p != NULL) pbuf_free(p);
    return ERR_VAL;
}

// Cleanup after HTTP POST finishes
void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    (void) connection;
    (void) response_uri;
    (void) response_uri_len;
}

// Handles deferred bootloader jumps or full system resets
static void process_pending_reboots(void) {
    if (reboot_to_bootsel_pending) {
#if PICOLAB_HAS_NEOPIXEL
        sleep_us(100); 
        status_led_show_connection_state(); 
#endif
        // Make sure the LED status changes before to jumping into UF2 bootloader
        sleep_ms(250);
        reset_usb_boot(0, 0);
    }

    if (reboot_normal_pending) {
        sleep_ms(250);
        watchdog_enable(1, 1);
        while (1);
    }
}

// Performs initial hardware GPIO states setup depending on the exact board configuration
static void board_runtime_init(void) {
#if PICOLAB_HAS_EXT_DIR_CTRL
    for (int i = 0; i < NUM_MANAGED_PINS; i++) {
        uint8_t ctrl_pin = PICOLAB_DIR_CTRL_PINS[i];
        if (ctrl_pin == PICOLAB_UNUSED_PIN) continue;
        gpio_init(ctrl_pin);
        gpio_set_dir(ctrl_pin, GPIO_OUT);
        gpio_put(ctrl_pin, !PICOLAB_DIR_OUT_LEVEL);
    }
#endif

#if PICOLAB_HAS_EXT_PULLUP_CTRL
    for (int i = 0; i < NUM_MANAGED_PINS; i++) {
        uint8_t ctrl_pin = PICOLAB_PULLUP_CTRL_PINS[i];
        if (ctrl_pin == PICOLAB_UNUSED_PIN) continue;
        gpio_init(ctrl_pin);
        gpio_set_dir(ctrl_pin, GPIO_OUT);
        gpio_put(ctrl_pin, !PICOLAB_PULLUP_ENABLE_LEVEL);
    }
#endif

    for (int i = 0; i < NUM_MANAGED_PINS; i++) {
        uint8_t pin = MANAGED_PINS[i];
        gpio_init(pin);
        board_set_pin_direction(i, false);
        board_set_pin_pull(i, false);
    }

#if PICOLAB_HAS_ADJ_RAIL
    rail_safe_startup();
    if (psu_adj_enabled) {
        rail_hw_enable(true);
    }
#endif

#if PICOLAB_HAS_BOOTSEL_BUTTON
    gpio_init(PICOLAB_BOOTSEL_BUTTON_PIN);
    gpio_set_dir(PICOLAB_BOOTSEL_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(PICOLAB_BOOTSEL_BUTTON_PIN);
#endif

#if PICOLAB_HAS_NEOPIXEL
    if (!status_led_init()) panic("Failed to initialize PicoLab status LED");
    (void)colored_status_led_set_on_with_color(PICO_COLORED_STATUS_LED_COLOR_FROM_RGB(0, 0, 0));
#endif
}

// Background task verifying if BOOTSEL button was held long enough to trigger bootloader restart
static void board_bootsel_button_task(void) {
#if PICOLAB_HAS_BOOTSEL_BUTTON
    static uint32_t pressed_since = 0;
    bool active = gpio_get(PICOLAB_BOOTSEL_BUTTON_PIN) == PICOLAB_BOOTSEL_BUTTON_ACTIVE_LEVEL;
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (active) {
        if (pressed_since == 0) pressed_since = now;
        if ((now - pressed_since) > 800) {
            reboot_to_bootsel_pending = true;
            pressed_since = now;
        }
    } else {
        pressed_since = 0;
    }
#endif
}

// Main routine (Core 0)
int main(void) {
    // Elevate main system clock to 192MHz and size up memory layout
    set_sys_clock_khz(192000, true); 
    detect_flash_size();

    // Prepare fundamental board API & initialize communication UARTs
    board_init();
    hw_uart_bridge_init();

    // Retrieve unique pico ID to assign distinct serial numbers
    pico_get_unique_board_id_string(global_serial_str, sizeof(global_serial_str));
    send_sysinfo_pending = true;

    board_runtime_init();
    
    // TinyUSB setup for virtual USB Ethernet functionality
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
    board_init_after_tusb();
    
    // Boot up Lightweight IP stack (lwIP) + HTTP Server + DNS/DHCP
    init_lwip();
    while (!netif_is_up(&netif_data));
    while (dhserv_init(&dhcp_config) != ERR_OK);
    while (dnserv_init(IP_ADDR_ANY, 53, dns_query_proc) != ERR_OK);
    
    httpd_init(); 
    http_set_cgi_handlers(cgi_handlers, LWIP_ARRAYSIZE(cgi_handlers));
    sse_server_init(); 
    
    // Core 1 launched once network initialization completes
    multicore_launch_core1(core1_entry);

    // Primary Core 0 Loop (Networking, CLI, Logging & System Admin)
    while (1) {
        tud_task();
        sys_check_timeouts(); 
        cli_usb_task();
        led_blinking_task();
        sse_telemetry_task();
        uart_tx_task(); 
        board_bootsel_button_task();
        usb_network_flush_tx();
        process_pending_reboots();
    }
}

// TinyUSB runtime suspension callbacks modifying status LED blink rates
void tud_mount_cb(void) { blink_interval_ms = BLINK_MOUNTED; }
void tud_umount_cb(void) { blink_interval_ms = BLINK_NOT_MOUNTED; }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; blink_interval_ms = BLINK_SUSPENDED; }
void tud_resume_cb(void) { blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED; }

// Required architectural LWIP stubs ensuring compatibilities 
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
uint32_t sys_now(void) { return to_ms_since_boot(get_absolute_time()); }