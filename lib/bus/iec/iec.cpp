#ifdef BUILD_IEC

#include "iec.h"

#include <cstring>
#include <memory>

#include "soc/io_mux_reg.h"
#include "driver/gpio.h"
#include "hal/gpio_hal.h"

#include "../../include/debug.h"
#include "../../include/pinmap.h"
#include "../../include/cbm_defines.h"
#include "led.h"
#include "led_strip.h"

#include "protocol/_protocol.h"
#include "protocol/cpbstandardserial.h"
#include "protocol/jiffydos.h"
#ifdef MEATLOAF_MAX
#include "protocol/saucedos.h"
#endif
#ifdef PARALLEL_BUS
#include "protocol/dolphindos.h"
#endif


#include "string_utils.h"
#include "utils.h"

#define MAIN_STACKSIZE	 4096
#define MAIN_PRIORITY	 20
#define MAIN_CPUAFFINITY 1

#define IEC_ALLDEV		 31
#define IEC_SET_STATE(x) ({ _state = x; })

using namespace Protocol;

systemBus IEC;

static void IRAM_ATTR cbm_on_atn_isr_forwarder(void *arg)
{
    systemBus *b = (systemBus *)arg;

    b->cbm_on_atn_isr_handler();
}

void IRAM_ATTR systemBus::cbm_on_atn_isr_handler()
{
    //IEC_ASSERT(PIN_IEC_SRQ);
    if (IEC_IS_ASSERTED(PIN_IEC_ATN))
    {
        //IEC_ASSERT(PIN_DEBUG);

        // Go to listener mode and get command
        IEC_RELEASE(PIN_IEC_CLK_OUT);
        IEC_ASSERT(PIN_IEC_DATA_OUT);

        flags = CLEAR;
        flags |= ATN_ASSERTED;
        IEC_SET_STATE(BUS_ACTIVE);

        gpio_intr_enable(PIN_IEC_CLK_IN);

        // Commands are always sent using standard serial
        if (detected_protocol != PROTOCOL_SERIAL)
        {
            detected_protocol = PROTOCOL_SERIAL;
            protocol = selectProtocol();
        }
        //IEC_RELEASE(PIN_DEBUG);
    }
    else if (_state == BUS_RELEASE)
    {
        releaseLines();
        IEC_SET_STATE(BUS_IDLE);
    }
    else
    {
        gpio_intr_disable(PIN_IEC_CLK_IN);
#ifdef JIFFYDOS
        if (flags & JIFFYDOS_ACTIVE)
        {
            //IEC_ASSERT(PIN_DEBUG);
            detected_protocol = PROTOCOL_JIFFYDOS;
            protocol = selectProtocol();
            //IEC_RELEASE(PIN_DEBUG);
        }
#endif
        sendInput();
    }
    //IEC_RELEASE(PIN_DEBUG);
}

static void IRAM_ATTR cbm_on_clk_isr_forwarder(void *arg)
{
    systemBus *b = (systemBus *)arg;

    b->cbm_on_clk_isr_handler();
}

void IRAM_ATTR systemBus::cbm_on_clk_isr_handler()
{
    IEC_ASSERT(PIN_DEBUG);
    int atn, val;
    int cmd, dev;

    if (_state < BUS_ACTIVE)
        return;

    atn = IEC_IS_ASSERTED(PIN_IEC_ATN);
    gpio_intr_disable(PIN_IEC_CLK_IN);

    //IEC_ASSERT(PIN_DEBUG);
    val = protocol->receiveByte();
    //IEC_RELEASE(PIN_DEBUG);

    if (flags & ERROR)
        goto done;

    if (atn)
    {
        cmd = val & 0xe0;
        dev = val & 0x1f;

        switch (cmd)
        {
        case IEC_LISTEN:
        case IEC_TALK:
            if (dev == IEC_ALLDEV || !isDeviceEnabled(dev))
            {
                if (dev == IEC_ALLDEV)
                {
                    // Handle releaseLines() when ATN is released outside of this
                    // interrupt to prevent watchdog timeout
                    IEC_SET_STATE(BUS_RELEASE);
                }
                else
                {
                    IEC_SET_STATE(BUS_IDLE);
                    protocol->transferDelaySinceLast(TIMING_Tbb);
                    releaseLines();
                }
                sendInput();
            }
            else
            {
                newIO(val);
            }
            break;

        case IEC_REOPEN:
            /* Take a break driver 8. We can reach our destination, but we're still a ways away */
            if (iec_curCommand)
            {
                channelIO(val);
                if (iec_curCommand->primary == IEC_TALK)
                {
                    IEC_SET_STATE(BUS_IDLE);
                    turnAround();
                    sendInput();
                }
            }
            break;

        case IEC_CLOSE:
            if (iec_curCommand)
            {
                channelIO(val);
                if (dev == 0x00)
                    sendInput();
            }
            break;

        default:
            break;
        }
    }

done:
    //gpio_intr_enable(PIN_IEC_CLK_IN);
    IEC_RELEASE(PIN_DEBUG);
    return;
}

#ifdef MEATLOAF_MAX
static void IRAM_ATTR cbm_on_data_isr_forwarder(void *arg)
{
    systemBus *b = (systemBus *)arg;

    b->cbm_on_data_isr_handler();
}

void IRAM_ATTR systemBus::cbm_on_data_isr_handler()
{
    // DATA
    if ( !IEC_IS_ASSERTED(PIN_IEC_ATN) && !IEC_IS_ASSERTED(PIN_IEC_CLK_IN) )
    {
        // 
        IEC_ASSERT(PIN_IEC_SRQ);

        // Wait for CLK to be asserted
        // If asserted within a certain time we are talking SauceDOS protocol
        if (!protocol->waitForSignals(PIN_IEC_CLK_IN, IEC_ASSERTED, 0, 0, TIMEOUT_DEFAULT))
        {
            // Stretch CLK to prepare to receive bits
            IEC_ASSERT(PIN_IEC_CLK_OUT);

            detected_protocol = PROTOCOL_SAUCEDOS;
            selectProtocol();
        }
    }
}
#endif

#ifdef IEC_HAS_RESET
static void IRAM_ATTR cbm_on_reset_isr_forwarder(void *arg)
{
    systemBus *b = (systemBus *)arg;

    b->cbm_on_reset_isr_handler();
}

void IRAM_ATTR systemBus::cbm_on_reset_isr_handler()
{
    if ( IEC_IS_ASSERTED(PIN_IEC_ATN) )
    {
        // RESET!
    }
}
#endif

void IRAM_ATTR systemBus::newIO(int val)
{
    iec_curCommand = new IECData();
    iec_curCommand->primary = val & 0xe0;
    iec_curCommand->device = val & 0x1f;
    iec_curCommand->payload = "";

    return;
}

void IRAM_ATTR systemBus::channelIO(int val)
{
    iec_curCommand->secondary = val & 0xf0;
    iec_curCommand->channel = val & 0x0f;
    return;
}

void IRAM_ATTR systemBus::sendInput(void)
{
    BaseType_t woken;

    //IEC_ASSERT(PIN_DEBUG);
    if (iec_curCommand)
    {
        xQueueSendFromISR(iec_commandQueue, &iec_curCommand, &woken);
    }
    iec_curCommand = nullptr;
    //IEC_RELEASE(PIN_DEBUG);

    return;
}


std::shared_ptr<IECProtocol> systemBus::selectProtocol()
{
    //Debug_printv("protocol[%d]", detected_protocol);

    switch (detected_protocol)
    {
#ifdef MEATLOAF_MAX
        case PROTOCOL_SAUCEDOS:
        {
            auto p = std::make_shared<SauceDOS>();
            return std::static_pointer_cast<IECProtocol>(p);
        }
#endif
#ifdef JIFFYDOS
        case PROTOCOL_JIFFYDOS:
        {
            auto p = std::make_shared<JiffyDOS>();
            return std::static_pointer_cast<IECProtocol>(p);
        }
#endif
#ifdef PARALLEL_BUS
        case PROTOCOL_DOLPHINDOS:
        {
            auto p = std::make_shared<DolphinDOS>();
            return std::static_pointer_cast<IECProtocol>(p);
        }
#endif
        default:
        {
#ifdef PARALLEL_BUS
            PARALLEL.state = PBUS_IDLE;
#endif
            auto p = std::make_shared<CPBStandardSerial>();
            return std::static_pointer_cast<IECProtocol>(p);
        }
    }
}

/**
 * Static callback function for the interrupt rate limiting timer. It sets the interruptProceed
 * flag to true. This is set to false when the interrupt is serviced.
 */
static void onTimer(void *info)
{
    systemBus *parent = (systemBus *)info;
    // portENTER_CRITICAL_ISR(&parent->timerMux);
    parent->interruptSRQ = !parent->interruptSRQ;
    // portEXIT_CRITICAL_ISR(&parent->timerMux);
}

static void ml_iec_intr_task(void* arg)
{
    while ( true )
    {
        if ( IEC.enabled )
            IEC.service();

        taskYIELD();
    }
}

void systemBus::init_gpio(gpio_num_t _pin)
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[_pin], PIN_FUNC_GPIO);
    gpio_set_direction(_pin, GPIO_MODE_INPUT);
    gpio_pullup_en(_pin);
    gpio_set_pull_mode(_pin, GPIO_PULLUP_ONLY);
    gpio_set_level(_pin, 0);
    return;
}

void systemBus::setup()
{
    Debug_printf("IEC systemBus::setup()\r\n");

    flags = CLEAR;
    protocol = selectProtocol();

    // initial pin modes in GPIO
    init_gpio(PIN_IEC_ATN);
    init_gpio(PIN_IEC_CLK_IN);
    init_gpio(PIN_IEC_CLK_OUT);
    init_gpio(PIN_IEC_DATA_IN);
    init_gpio(PIN_IEC_DATA_OUT);
    init_gpio(PIN_IEC_SRQ);
#ifdef IEC_HAS_RESET
    init_gpio(PIN_IEC_RESET);
#endif

#ifdef IEC_INVERTED_LINES
#warning intr_type likely needs to be fixed!
#endif

    iec_commandQueue = xQueueCreate(10, sizeof(IECData *));

    // Start task
    // Create a new high-priority task to handle the main service loop
    // This is assigned to CPU1; the WiFi task ends up on CPU0
    xTaskCreatePinnedToCore(ml_iec_intr_task, "ml_iec_intr_task", MAIN_STACKSIZE, NULL, MAIN_PRIORITY, NULL, MAIN_CPUAFFINITY);

    // Setup interrupt for ATN
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_IEC_ATN), // bit mask of the pins that you want to set
        .mode = GPIO_MODE_INPUT,			   // set as input mode
        .pull_up_en = GPIO_PULLUP_DISABLE,	   // disable pull-up mode
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // disable pull-down mode
        .intr_type = GPIO_INTR_ANYEDGE		   // interrupt of any edge
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)PIN_IEC_ATN, cbm_on_atn_isr_forwarder, this);

    // Setup interrupt config for CLK
    io_conf = {
        .pin_bit_mask = (1ULL << PIN_IEC_CLK_IN),   // bit mask of the pins that you want to set
        .mode = GPIO_MODE_INPUT,                    // set as input mode
        .pull_up_en = GPIO_PULLUP_DISABLE,          // disable pull-up mode
        .pull_down_en = GPIO_PULLDOWN_DISABLE,      // disable pull-down mode
#ifdef IEC_INVERTED_LINES
        .intr_type = GPIO_INTR_NEGEDGE              // interrupt of falling edge
#else
        .intr_type = GPIO_INTR_POSEDGE              // interrupt of rising edge
#endif
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)PIN_IEC_CLK_IN, cbm_on_clk_isr_forwarder, this);
    gpio_intr_disable(PIN_IEC_CLK_IN);

#ifdef MEATLOAF_MAX
    // Setup interrupt config for DATA
    io_conf = {
        .pin_bit_mask = (1ULL << PIN_IEC_DATA_IN),   // bit mask of the pins that you want to set
        .mode = GPIO_MODE_INPUT,                    // set as input mode
        .pull_up_en = GPIO_PULLUP_DISABLE,          // disable pull-up mode
        .pull_down_en = GPIO_PULLDOWN_DISABLE,      // disable pull-down mode
#ifdef IEC_INVERTED_LINES
        .intr_type = GPIO_INTR_NEGEDGE              // interrupt of falling edge
#else
        .intr_type = GPIO_INTR_POSEDGE              // interrupt of rising edge
#endif
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)PIN_IEC_DATA_IN, cbm_on_data_isr_forwarder, this);
#endif

#ifdef IEC_HAS_RESET
    // Setup interrupt config for RESET
    io_conf = {
        .pin_bit_mask = (1ULL << PIN_IEC_RESET),   // bit mask of the pins that you want to set
        .mode = GPIO_MODE_INPUT,                    // set as input mode
        .pull_up_en = GPIO_PULLUP_DISABLE,          // disable pull-up mode
        .pull_down_en = GPIO_PULLDOWN_DISABLE,      // disable pull-down mode
        .intr_type = GPIO_INTR_NEGEDGE              // interrupt of falling edge
    };
    gpio_config(&io_conf);
    gpio_isr_handler_add((gpio_num_t)PIN_IEC_RESET, cbm_on_reset_isr_forwarder, this);
#endif

    // Start SRQ timer service
    //timer_start_srq();
}

void IRAM_ATTR systemBus::service()
{
    IECData *received;

    if (!xQueueReceive(iec_commandQueue, &received, 0))
        return;

    // Read Payload
    if (received->primary == IEC_LISTEN && received->secondary != IEC_CLOSE)
    {
        //IEC_ASSERT(PIN_DEBUG);
        received->payload = protocol->receiveBytes();
        //Debug_printv("primary[%02X] secondary[%02X] payload[%s]", received->primary, received->secondary, received->payload.c_str());
        //IEC_RELEASE(PIN_DEBUG);
    }

    if (flags & JIFFYDOS_ACTIVE)
        Debug_println("JiffyDOS!");

    received->debugPrint();

    auto d = deviceById(received->device);
    if (d != nullptr)
    {
        d->commanddata = *received;
        d->process();
    }

    // Command was processed, clear it out
    delete received;

    return;
}

/**
 * Start the Interrupt rate limiting timer
 */
void systemBus::timer_start_srq()
{
    esp_timer_create_args_t tcfg;
    tcfg.arg = this;
    tcfg.callback = onTimer;
    tcfg.dispatch_method = esp_timer_dispatch_t::ESP_TIMER_TASK;
    tcfg.name = nullptr;
    esp_timer_create(&tcfg, &rateTimerHandle);
    esp_timer_start_periodic(rateTimerHandle, timerRate * 1000);
}

/**
 * Stop the Interrupt rate limiting timer
 */
void systemBus::timer_stop_srq()
{
    // Delete existing timer
    if (rateTimerHandle != nullptr)
    {
        Debug_println("Deleting existing rateTimer\r\n");
        esp_timer_stop(rateTimerHandle);
        esp_timer_delete(rateTimerHandle);
        rateTimerHandle = nullptr;
    }
}


systemBus virtualDevice::get_bus() { return IEC; }

device_state_t virtualDevice::process()
{
    switch ((bus_command_t)commanddata.primary)
    {
    case bus_command_t::IEC_LISTEN:
        switch (commanddata.secondary)
        {
        case bus_command_t::IEC_OPEN:
            payload = commanddata.payload;
            state = openChannel(/*commanddata.channel, payload*/);
            break;
        case bus_command_t::IEC_CLOSE:
            state = closeChannel(/*commanddata.channel*/);
            break;
        case bus_command_t::IEC_REOPEN:
            payload = commanddata.payload;
            state = writeChannel(/*commanddata.channel, payload*/);
            break;
        }
        break;

    case bus_command_t::IEC_TALK:
        if (commanddata.secondary == bus_command_t::IEC_REOPEN)
            state = readChannel(/*commanddata.channel*/);
        break;

    default:
        break;
    }

    return state;
}

// This is only used in iecDrive, and it has its own implementation
void virtualDevice::iec_talk_command_buffer_status()
{
    // Removed implementation as it wasn't being used
}

void virtualDevice::dumpData()
{
    Debug_printf("%9s: %02X\r\n", "Primary", commanddata.primary);
    Debug_printf("%9s: %02u\r\n", "Device", commanddata.device);
    Debug_printf("%9s: %02X\r\n", "Secondary", commanddata.secondary);
    Debug_printf("%9s: %02u\r\n", "Channel", commanddata.channel);
    Debug_printf("%9s: %s\r\n", "Payload", commanddata.payload.c_str());
}

void systemBus::assert_interrupt()
{
    if (interruptSRQ)
        IEC_ASSERT(PIN_IEC_SRQ);
    else
        IEC_RELEASE(PIN_IEC_SRQ);
}

uint8_t systemBus::receiveByte() { return protocol->receiveByte(); }
std::string systemBus::receiveBytes() { return protocol->receiveBytes(); }

bool systemBus::sendByte(const char c, bool eoi) { return protocol->sendByte(c, eoi); }
size_t systemBus::sendBytes(const char *buf, size_t len, bool eoi) { return protocol->sendBytes(buf, len, eoi); }
size_t systemBus::sendBytes(std::string s, bool eoi) { return protocol->sendBytes(s.c_str(), s.size(), eoi); }

bool IRAM_ATTR systemBus::turnAround()
{
    /*
    TURNAROUND
    An unusual sequence takes place following ATN if the computer
    wishes the remote device to become a talker. This will usually
    take place only after a Talk command has been sent.  Immediately
    after ATN is RELEASED, the selected device will be behaving like a
    listener. After all, it's been listening during the ATN cycle, and
    the computer has been a talker. At this instant, we have "wrong
    way" logic; the device is asserting the Data line, and the
    computer is asserting the Clock line. We must turn this around.

    Here's the sequence:

    1. The computer asserts the Data line (it's already there), as
       well as releases the Clock line.

    2. When the device sees the Clock line is releaseed, it releases
       the Data line (which stays asserted anyway since the computer
       is asserting it) and then asserts the Clock line.

    We're now in our starting position, with the talker (that's the
    device) asserting the Clock, and the listener (the computer)
    asserting the Data line. The computer watches for this state;
    only when it has gone through the cycle correctly will it be ready
    to receive data. And data will be signalled, of course, with the
    usual sequence: the talker releases the Clock line to signal that
    it's ready to send.
    */

    // Wait for ATN to be released
    if (protocol->waitForSignals(PIN_IEC_ATN, IEC_RELEASED, 0, 0, FOREVER) == TIMED_OUT)
    {
        flags |= ERROR;
        return false;
    }

    // Wait for CLK to be released
    if (protocol->waitForSignals(PIN_IEC_CLK_IN, IEC_RELEASED, 0, 0, TIMEOUT_Ttlta) == TIMED_OUT)
    {
        flags |= ERROR;
        return false; // return error because timeout
    }
    IEC_RELEASE(PIN_IEC_DATA_OUT);
    usleep(TIMING_Ttca);
    IEC_ASSERT(PIN_IEC_CLK_OUT);

    // 80us minimum delay after TURNAROUND
    // *** IMPORTANT!
    usleep(TIMING_Tda);

    return true;
} // turnAround

void systemBus::reset_all_our_devices()
{
    // TODO iterate through our bus and send reset to each device.

    // The reset line on the IEC bus signals all of the drives on the bus to reset. 
    // Each drive is a separate computer with its own power system. Therefore, it 
    // will often be the case that a drive has been on, it has been used to load a 
    // program, but then the computer gets power cycled. This does not cause the 
    // drives to power cycle, but because they get that reset signal they will jump 
    // through their own initialization procedure. On a CMD storage device, or an 
    // SD2IEC device, this means that they will take certain steps like, unmount all 
    // mounted .D64 images, unassign any assigned disk image swaplist, revert the 
    // current path of all partitions to the root directory, and revert the current 
    // partition to the drive's default partition. The error channel is cleared and 
    // replaced with a message about the drive's DOS version. Drives may also clear 
    // out certain cached data, such as the disk's BAM. And of course, all open files 
    // are automatically closed.
}

void systemBus::setBitTiming(std::string set, int p1, int p2, int p3, int p4)
{
     uint8_t i = 0; // Send
     if (mstr::equals(set, (char *) "r")) i = 1;
    if (p1) protocol->bit_pair_timing[i][0] = p1;
    if (p2) protocol->bit_pair_timing[i][1] = p2;
    if (p3) protocol->bit_pair_timing[i][2] = p3;
    if (p4) protocol->bit_pair_timing[i][3] = p4;

    Debug_printv("i[%d] timing[%d][%d][%d][%d]", i,
                    protocol->bit_pair_timing[i][0], 
                    protocol->bit_pair_timing[i][1], 
                    protocol->bit_pair_timing[i][2], 
                    protocol->bit_pair_timing[i][3]);
}

void IRAM_ATTR systemBus::releaseLines(bool wait)
{
    // Release lines
    IEC_RELEASE(PIN_IEC_CLK_OUT);
    IEC_RELEASE(PIN_IEC_DATA_OUT);
    IEC_SET_STATE(BUS_IDLE);

    // Wait for ATN to release and quit
    if (wait)
    {
        Debug_printv("Waiting for ATN to release");
        protocol->waitForSignals(PIN_IEC_ATN, IEC_RELEASED, 0, 0, TIMEOUT_DEFAULT);
    }
}

void IRAM_ATTR systemBus::senderTimeout()
{
    releaseLines();
    IEC_SET_STATE(BUS_ERROR);

    usleep(TIMING_EMPTY);
    IEC_ASSERT(PIN_IEC_DATA_OUT);
} // senderTimeout

void systemBus::addDevice(virtualDevice *pDevice, int device_id)
{
    if (!pDevice)
    {
        Debug_printf("systemBus::addDevice() pDevice == nullptr! returning.\r\n");
        return;
    }

    // TODO, add device shortcut pointer logic like others
    printf("Device #%02d Ready!\r\n", device_id);

    pDevice->_devnum = device_id;
    _daisyChain.push_front(pDevice);
    enabledDevices |= 1UL << device_id;
}

void systemBus::remDevice(virtualDevice *pDevice)
{
    if (!pDevice)
    {
        Debug_printf("system Bus::remDevice() pDevice == nullptr! returning\r\n");
        return;
    }

    _daisyChain.remove(pDevice);
    enabledDevices &= ~(1UL << pDevice->_devnum);
}

bool systemBus::isDeviceEnabled(const uint8_t device_id) { return (enabledDevices & (1 << device_id)); } // isDeviceEnabled

void systemBus::changeDeviceId(virtualDevice *pDevice, int device_id)
{
    if (!pDevice)
    {
        Debug_printf("systemBus::changeDeviceId() pDevice == nullptr! returning.\r\n");
        return;
    }

    for (auto devicep : _daisyChain)
    {
        if (devicep == pDevice)
            devicep->_devnum = device_id;
    }
}

virtualDevice *systemBus::deviceById(int device_id)
{
    for (auto devicep : _daisyChain)
    {
        if (devicep->_devnum == device_id)
            return devicep;
    }
    return nullptr;
}

void systemBus::shutdown()
{
    shuttingDown = true;

    for (auto devicep : _daisyChain)
    {
        Debug_printf("Shutting down device #%02d\r\n", devicep->id());
        devicep->shutdown();
    }
    Debug_printf("All devices shut down.\r\n");
}

enum
{
    IECOpenChannel = 0,
    IECCloseChannel,
    IECReadChannel,
    IECWriteChannel,
};

static const char *IECCommandNames[] = {
    "UNKNOWN", "LISTEN", "TALK", "REOPEN", "OPEN", "CLOSE", "READ", "WRITE",
};

int IECData::channelCommand()
{
    switch (primary)
    {
    case IEC_LISTEN:
        switch (secondary)
        {
        case IEC_OPEN:
            return IECOpenChannel;
        case IEC_CLOSE:
            return IECCloseChannel;
        case IEC_REOPEN:
            return IECWriteChannel;
        }
        break;

    case IEC_TALK:
        return IECReadChannel;
    }

    return 0;
}

void IECData::debugPrint()
{
    //Debug_printf("IEC %2i %-5s %2i: %-6s [%02X %02X]\r\n", device, IECCommandNames[channelCommand() + 4], channel, IECCommandNames[primary >> 5], primary, secondary);
    Debug_printf("IEC [%02X %02X] %-6s %02d %-5s %02d \r\n", 
        primary,
        secondary,
        IECCommandNames[primary >> 5],
        device,
        IECCommandNames[channelCommand() + 4],
        channel
    );
    if (payload.size())
        Debug_printf("%s", util_hexdump(payload.data(), payload.size()).c_str());
}

#endif /* BUILD_IEC */
