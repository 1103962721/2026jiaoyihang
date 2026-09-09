# 2K Role Dispatch and Client Height Design

**Date:** 2026-07-11

## Approved scope

- Keep the approved teal client palette and current card composition unchanged.
- Apply option A to the client: increase the default/minimum height and prevent the top connection-status badge from being squeezed vertically.
- Use a 2560 x 1440 game-client-area reference coordinate system.
- Bullet OCR defaults become rectangle `(2047, 1126, 400, 80)` while preserving all existing OCR parsing, retry, and `recognized price / bullet total <= unit price` behavior.
- Coin-role dispatch clicks `(1756, 1000)`; boss-role dispatch clicks `(2262, 1159)`. Both are scaled independently by current client width and height.
- The server sends a role command to the client. The client stores the current role and chooses the fixed point locally; the protocol never accepts arbitrary click coordinates.
- "Dispatch all" sends the first role group in one tight loop, waits the configured inter-group delay using a precise Qt timer, then sends the second role group in one tight loop. The selected coin-first/boss-first radio controls ordering.
- Listing remains a placeholder and only counts/targets coin-role clients.

## Compatibility decisions

- Existing F3 OCR hotkey and OCR string normalization remain unchanged.
- The legacy retry point keeps its own 1650 x 1080 reference size so changing the OCR rectangle reference does not silently move the retry click.
- A newly connected client defaults to coin and the server immediately synchronizes that role. Later role changes are also pushed immediately.
- Single-client dispatch uses the client's synchronized role and the same fixed-point execution path.
