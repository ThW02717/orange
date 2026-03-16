## Project Invariant

- Target board is OrangePi RV2 (not VF2).
- If provided specs are from other board versions (for example VF2), automatically map and apply them to OrangePi RV2.
- Assume deployment/testing is on real hardware (power-on board), not only in emulation.
- Assume kernel delivery uses the UART bootloader flow to send the kernel to the board SD-card boot environment; prioritize debugging and suggestions that match this board-side workflow.
