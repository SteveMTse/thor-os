//=======================================================================
// Copyright Baptiste Wicht 2013-2016.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================

#include "rtl8139.hpp"
#include "logging.hpp"
#include "kernel_utils.hpp"
#include "physical_allocator.hpp"
#include "virtual_allocator.hpp"
#include "interrupts.hpp"
#include "paging.hpp"

#include "ethernet_layer.hpp"

#define MAC0 0x00
#define MAC4 0x04
#define CMD 0x37
#define IMR 0x3C //Interrupt mask register
#define ISR 0x3E //Interrupt status register
#define RCR 0x44 //Receive Config Register
#define CONFIG_1 0x52
#define RX_BUF 0x30
#define RX_BUF_PTR 0x38
#define RX_BUF_ADDR 0x3A

#define RX_MISSED 0x4C
#define RX_OK 0x01
#define CMD_NOT_EMPTY 0x01

#define RCR_AAP  (1 << 0) /* Accept All Packets */
#define RCR_APM  (1 << 1) /* Accept Physical Match Packets */
#define RCR_AM   (1 << 2) /* Accept Multicast Packets */
#define RCR_AB   (1 << 3) /* Accept Broadcast Packets */
#define RCR_WRAP (1 << 7) /* Wrap packets too long */

#define RX_STATUS_OK 0x1
#define RX_BAD_ALIGN 0x2
#define RX_CRC_ERR 0x4
#define RX_TOO_LONG 0x8
#define RX_RUNT 0x10
#define RX_BAD_SYMBOL 0x20
#define RX_BROADCAST 0x2000
#define RX_PHYSICAL 0x4000
#define RX_MULTICAST 0x8000

namespace {

struct rtl8139_t {
    uint32_t iobase;
    uint64_t phys_buffer_rx;
    uint64_t buffer_rx;

    uint64_t cur_rx; //Index inside the buffer
};

void packet_handler(interrupt::syscall_regs*, void* data){
    logging::logf(logging::log_level::TRACE, "rtl8139: Packet Received\n");

    auto& desc = *static_cast<rtl8139_t*>(data);

    // Get the interrupt status
    auto status = in_word(desc.iobase + ISR);

    // Acknowledge the handling of the packet
    out_word(desc.iobase + ISR, status);

    if(status & RX_OK){
        logging::logf(logging::log_level::TRACE, "rtl8139: Receive status OK\n");

        auto cur_rx = desc.cur_rx;

        while((in_byte(desc.iobase + CMD) & CMD_NOT_EMPTY) == 0){
            auto cur_offset = cur_rx % 0x3000;
            auto buffer_rx = reinterpret_cast<char*>(desc.buffer_rx);

            auto packet_status = *reinterpret_cast<uint32_t*>(buffer_rx + cur_offset);
            auto packet_length = packet_status >> 16; //Extract the size from the header
            auto packet_payload = buffer_rx + cur_offset + 4; //Skip the packet header (NIC)

            if (packet_status & (RX_BAD_SYMBOL | RX_RUNT | RX_TOO_LONG | RX_CRC_ERR | RX_BAD_ALIGN)) {
                logging::logf(logging::log_level::TRACE, "rtl8139: Packet Error, status:%u\n", uint64_t(packet_status));

                //TODO We should probably reset the controller ?
            } else if(packet_length == 0){
                // TODO Normally this should not happen, it probably indicates a bug somewhere
                logging::logf(logging::log_level::TRACE, "rtl8139: Packet Error Length = 0, status:%u\n", uint64_t(packet_status));
            } else {
                // Omit CRC from the length
                auto packet_only_length = packet_length - 4;

                logging::logf(logging::log_level::TRACE, "rtl8139: Packet OK length:%u\n", uint64_t(packet_only_length));

                auto packet_buffer = new char[packet_only_length];

                std::copy_n(packet_buffer, packet_payload, packet_only_length);

                network::ethernet::packet packet(packet_buffer, packet_only_length);
                network::ethernet::decode(packet);

                delete[] packet_buffer;
            }

            cur_rx = (cur_rx + packet_length + 4 + 3) & ~3; //align on 4 bytes
            out_word(desc.iobase + RX_BUF_PTR, cur_rx - 0x10);

            logging::logf(logging::log_level::TRACE, "rtl8139: Packet Handled\n");
        }

        desc.cur_rx = cur_rx;
    } else {
        logging::logf(logging::log_level::TRACE, "rtl8139: Receive status not OK\n");
    }
}

} //end of anonymous namespace

void rtl8139::init_driver(network::interface_descriptor& interface, pci::device_descriptor& pci_device){
    logging::logf(logging::log_level::TRACE, "rtl8139: Initialize RTL8139 driver on pci:%u:%u:%u\n", uint64_t(pci_device.bus), uint64_t(pci_device.device), uint64_t(pci_device.function));

    rtl8139_t* desc = new rtl8139_t();
    interface.driver_data = desc;

    // 1. Enable PCI Bus Mastering (allows DMA)

    auto command_register = pci::read_config_dword(pci_device.bus, pci_device.device, pci_device.function, 0x4);
    command_register |= 0x4; // Set Bus Mastering Bit
    pci::write_config_dword(pci_device.bus, pci_device.device, pci_device.function, 0x4, command_register);

    // 2. Get the I/O base address

    auto iobase = pci::read_config_dword(pci_device.bus, pci_device.device, pci_device.function, 0x10) & (~0x3);
    desc->iobase = iobase;

    logging::logf(logging::log_level::TRACE, "rtl8139: I/O Base address :%h\n", uint64_t(iobase));

    // 3. Power on the device

    out_byte(iobase + CONFIG_1, 0x0);

    // 4. Software reset

    out_byte(iobase + CMD, 0x10);
    while( (in_byte(iobase + CMD) & 0x10) != 0) { /* Wait for RST to be done */ }

    // 5. Init the receive buffer

    auto buffer_rx_phys = physical_allocator::allocate(3);
    out_dword(iobase + RX_BUF, buffer_rx_phys);
    out_dword(iobase + RX_BUF_PTR, 0);
    out_dword(iobase + RX_BUF_ADDR, 0);

    auto buffer_rx_virt = virtual_allocator::allocate(3);
    if(!paging::map_pages(buffer_rx_virt, buffer_rx_phys, 3)){
        logging::logf(logging::log_level::ERROR, "rtl8139: Unable to map %h into %h\n", buffer_rx_phys, buffer_rx_virt);
    }

    desc->phys_buffer_rx = buffer_rx_phys;
    desc->buffer_rx = buffer_rx_virt;
    desc->cur_rx = 0;

    std::fill_n(reinterpret_cast<char*>(desc->buffer_rx), 0x3000, 0);

    logging::logf(logging::log_level::TRACE, "rtl8139: Physical RX Buffer :%h\n", uint64_t(desc->phys_buffer_rx));
    logging::logf(logging::log_level::TRACE, "rtl8139: Virtual RX Buffer :%h\n", uint64_t(desc->buffer_rx));

    // 6. Register IRQ handler

    auto irq = pci::read_config_dword(pci_device.bus, pci_device.device, pci_device.function, 0x3c) & 0xFF;
    interrupt::register_irq_handler(irq, packet_handler, desc);

    // 7. Set IMR + ISR

    logging::logf(logging::log_level::TRACE, "rtl8139: IRQ :%u\n", uint64_t(irq));

    out_word(iobase + IMR, 0x0005); // Sets the TOK and ROK bits high

    // 8. Set RCR (Receive Configuration Register)

    out_dword(iobase + RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    // 9. Enable RX and TX

    out_dword(iobase+ RX_MISSED, 0x0);
    out_byte(iobase + CMD, 0x0C); // Sets the RE and TE bits high

    // 10. Get the mac address

    size_t mac = 0;

    for(size_t i = 0; i < 6; ++i){
        mac |= uint64_t(in_byte(iobase + i)) << ((5 - i) * 8);
    }

    interface.mac_address = mac;

    logging::logf(logging::log_level::TRACE, "rtl8139: MAC Address %h n", mac);
}