.. _google_pda:

Twinkie
######

Overview
********

This provides access to :ref:`Twinkie <google_twinkie_v2>` through a serial
shell and console so you can try out the supported features.

A decoding application is required to convert the data packet sent out by
twinkie into a legible format.

Building and Running
********************

Build and flash Twinkie as follows:

.. zephyr-app-commands::
   :zephyr-app: samples/boards/google_pda
   :board: google_twinkie_v2
   :goals: build flash
   :compact:

After flashing, the LED will start red. Inputting the start command to the shell
will start the twinkie snooper.

Sample Usage
============

.. code-block:: shell

    uart:~$ start
    uart:~$ output pd_only
