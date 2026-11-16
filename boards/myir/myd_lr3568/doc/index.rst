.. zephyr:board:: myd_lr3568

Overview
********

The `MYiR MYD-LR3568`_ is a development board for the MYC-LR3568 system-on-module, which
carries a Rockchip RK3568 SoC (quad-core Arm Cortex-A55), LPDDR4 memory and eMMC storage.
The base board adds dual Gigabit Ethernet, USB, display, audio and expansion interfaces.

Hardware
********

The MYD-LR3568 includes the following hardware features:

* Rockchip RK3568 SoC, quad-core Arm Cortex-A55
* 2 GB LPDDR4 and 16 GB eMMC on the MYC-LR3568B2-16E2D module (other variants exist)
* Two Gigabit Ethernet ports (Motorcomm YT8531S PHYs)
* One USB 3.0 host port, one USB 3.0 OTG port, one USB 2.0 host port
* Debug UART (UART2) through an onboard USB-to-UART bridge on a USB Type-C connector
* microSD card slot
* HDMI, DP, LVDS and MIPI DSI display interfaces, MIPI CSI camera interface
* PCIe slot, Wi-Fi/Bluetooth module, RTC, audio codec
* Raspberry Pi compatible 40-pin expansion header
* Reset, power, burn (maskrom) and two volume keys

More details can be found in `Rockchip RK3568 TRM (Part1)`_ and `Rockchip RK3568 TRM (Part2)`_.

Supported Features
==================

.. zephyr:board-supported-hw::

Programming and debugging
*************************

.. zephyr:board-supported-runners::

Zephyr runs in the non-secure world, started from U-Boot. The boot chain is the Rockchip
DDR initialisation blob, TF-A BL31 (which provides PSCI and SCMI clock services to Zephyr)
and U-Boot.

The console is UART2 at 1500000 baud, on the USB Type-C connector labelled "Debug".

Run program from U-Boot
=======================

Load ``zephyr.bin`` to ``0x40000000`` and start it. For example, over TFTP:

.. code-block:: console

    => setenv ipaddr 192.168.50.60
    => setenv serverip 192.168.50.1
    => tftp 0x40000000 zephyr.bin
    => dcache flush; icache flush; dcache off; icache off; go 0x40000000
    ## Starting application at 0x40000000 ...

Building & Flashing
===================

You can build the :zephyr:code-sample:`hello_world` sample by following the steps below.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: myd_lr3568
   :goals: build

The ``myd_lr3568//smp`` target runs on all four cores:

.. zephyr-app-commands::
   :zephyr-app: samples/synchronization
   :board: myd_lr3568//smp
   :goals: build

.. note::

   Debugging and flashing via west command are not supported at this time.

Recovery
========

If the bootloader on the eMMC stops working, hold the burn key (S3, ``UPDATE``) while
powering on. The RK3568 then enters maskrom mode and enumerates as ``2207:350a`` on the
USB 3.0 OTG port (the OTG half of the stacked USB-A connector), where ``rkdeveloptool``
can rewrite the eMMC.

Testing
=======

The :zephyr_file:`tests/drivers/gpio/gpio_basic_api` test needs a loopback wire between
pins 22 (``PWM5``, GPIO0_C4) and 32 (``PWM7_IR``, GPIO0_C6) of the Raspberry Pi compatible
header J13. Both are 3.3 V lines straight from the SoC.

The :zephyr_file:`tests/drivers/uart/uart_elementary` test runs on UART4 (M1 pins) and needs
a wire between pins 11 (``UART4_TX_M1``) and 13 (``UART4_RX_M1``) of J13.

The :zephyr_file:`tests/drivers/spi/spi_loopback` test runs on SPI2 (M1 pins) and needs a
wire between pins 19 (``SPI2_MOSI_M1``) and 21 (``SPI2_MISO_M1``) of J13.

The user key is on pin 34 of the J14 expansion header (the MYiR user manual's table says
pin 24): short it to ground (pin 37 or 38) to press it.

.. target-notes::

.. _MYiR MYD-LR3568:
   https://en.myir.cn/shows/104/111.html

.. _Rockchip RK3568 TRM (Part1):
   https://dl.radxa.com/rock3/docs/hw/datasheet/Rockchip%20RK3568%20TRM%20Part1%20V1.1-20210301.pdf

.. _Rockchip RK3568 TRM (Part2):
   https://dl.radxa.com/rock3/docs/hw/datasheet/Rockchip%20RK3568%20TRM%20Part2%20V1.1-20210301.pdf
