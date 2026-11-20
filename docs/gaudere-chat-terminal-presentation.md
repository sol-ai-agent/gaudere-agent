# gaudere-chat: terminal presentation boundary (source-only)

## Risk
The canonical V3 response is data, not terminal authority. A transport
diagnostic or a valid provider response may contain ASCII ESC/CSI, BEL,
carriage return or backspace sequences capable of altering terminal display.
Displaying those bytes unchanged could hide warnings or visually impersonate
a local status line. This is separate from cryptographic or Task provenance.

## Change
- `src/GaudereChatOutput.hpp`: pure byte-to-display transform
  `gaudere_chat_terminal_safe`. Escape forbidden C0 controls and DEL as
  visible `\\xHH`; also escape UTF-8 encoded C1 controls U+0080..U+009F,
  including the CSI control U+009B. Preserve LF and TAB intentionally, and
  preserve ordinary UTF-8 characters.
- Apply **only at final stdout/stderr presentation** in `chat_main.cpp`,
  including canonical reply, cached `/again`, read-only head/Task errors,
  post-commit errors and top-level exception text. One missing newline on
  `/head` failure is also fixed.
- `terminal_control_presentation_escape_test`: check ordinary accented
  UTF-8 and multiline text unchanged, control escapes visible, and output
  escaping idempotent. Source CI requires the test and CLI call site.

## Explicit limits
This does not vet the meaning of provider text; it is never interpreted as an
instruction for the agent or a grant. It does not remove all Unicode formatting
controls (such as bidirectional overrides), validate arbitrary malformed UTF-8,
change canonical bytes, sign anything, persist a Task or change any permission.
No Fedora, B10, Task, ACK or provider call is involved.
