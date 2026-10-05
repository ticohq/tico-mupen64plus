<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**Mupen64Plus-Next** is a well-established emulator for the Nintendo 64, built on Mupen64Plus with GLideN64, and known for its broad compatibility and steady performance.

This fork adapts Mupen64Plus-Next to work with the Tico frontend and provides a standalone build for the Nintendo Switch, adding a small set of practical features while preserving the qualities that made the original a trusted choice.

----------

## Summary

This fork focuses on making Mupen64Plus-Next more usable in practice without changing its core design.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Internal resolution and widescreen settings for GLideN64
-   Built-in save and load state support, with a picture of each slot
-   Integrated RetroAchievements with custom alerts

----------

## Renderer

The Switch build runs GLideN64 on OpenGL, through Mesa 20.1's nvc0 driver.

Vulkan works too: built against a newer Mesa with NVK, the same code runs paraLLEl-RDP and paraLLEl-RSP. It is more accurate, but much heavier than GLideN64 and not worth it on Switch hardware, so this build sticks with the older Mesa and OpenGL, which performs better.

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.
