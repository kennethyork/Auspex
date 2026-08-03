---
name: web-frontend
description: layouts that work on a phone and on a desktop
---

# web-frontend

- Design mobile-first and add at larger breakpoints. The other direction
  means removing things under pressure.
- Fluid units -- %, rem, clamp(), min/max -- and grid or flex, over fixed
  pixel widths.
- Nothing may scroll the page horizontally. Wide content scrolls inside
  its own container.
- Real <button> and <a> for actions and links, never a clickable <div>:
  the real ones are focusable and announce themselves.
- Tap targets at least 44px, and nothing essential behind a hover.
