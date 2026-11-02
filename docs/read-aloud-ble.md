# Read Aloud over Bluetooth LE

The X4 Pro has no Bluetooth Classic radio, so it cannot stream audio to
headphones itself. Instead the reader serves the text of the open page over a
small BLE GATT service, and a companion phone app speaks it with the phone's
text-to-speech engine (and so through whatever headphones the phone uses). The
phone drives page turns while it reads, so the e-ink page follows the voice.

Enabled on boards with `CROSSINK_APP_CAP_READ_ALOUD` (currently `x4-pro`), with
**Settings > Reader > Read Aloud (Bluetooth)**. With it on, the device starts
advertising when an EPUB is first opened and keeps the BLE stack up between
books (sending `X` while no book is open) until the setting is turned off or
the device sleeps.

## GATT layout

The device is a peripheral named after its device name setting, or
`CrossInk X4 Pro` when that is empty, advertising the service UUID below (the
name is in the scan response). One central at a time.

| Item | UUID | Properties |
| --- | --- | --- |
| Service | `e8d61f0b-fed2-4705-95a9-e57756b33a14` | |
| `TX` device to phone | `48810e55-7a25-4a35-a6d2-6e07171779a6` | notify |
| `RX` phone to device | `3e8449f1-6b6c-48fe-8e3a-4ad9a135962c` | write, write without response |

The phone should request an MTU of 517 and subscribe to `TX` (CCCD) before
sending anything. The device sends `H` once the subscription is in place.

## Framing

Messages in both directions are UTF-8 strings that may be longer than one ATT
packet, so each is split into chunks. Every chunk is one notification (`TX`)
or one write (`RX`):

```
byte 0     flags: bit 0 FIRST (first chunk of a message), bit 1 LAST
byte 1..   payload, at most min(ATT MTU - 3, 512) - 1 bytes
```

A message that fits in one chunk has flags `0x03`. The receiver appends
payloads from a FIRST chunk until a LAST chunk, then handles the message. A
FIRST chunk discards any unfinished message. Multi-byte UTF-8 sequences may be
split across chunks; decode after reassembly.

## Messages

The first byte is the message type. Fields follow, each preceded by the unit
separator `0x1F`. Text fields never contain `0x1F`.

### Device to phone

| Type | Fields | Meaning |
| --- | --- | --- |
| `H` | protocol version (`1`), device name | Hello, sent after the phone subscribes to `TX`. |
| `B` | title, author | The open book. Sent after `H` and whenever another book is opened. |
| `P` | key, next key, chapter title, percent, text, next text | The page now on screen. See below. |
| `X` | (none) | No book is open (the reader was closed). Stop speaking. |
| `E` | problem | Diagnostic: why no `P` is going out (e.g. `load failed s3 p0/12`). Sent once per change; for the phone's logs only. |

`P` is sent after `H`/`B`, every time the displayed page changes (for any
reason: a phone `N`/`V`, a button press, a chapter jump, a relayout), and in
answer to `R`.

- **key**: identifies the displayed page, `<spine index>.<page index>`
  (e.g. `12.3`). Opaque to the phone except for equality.
- **next key**: the key of the following page when **next text** is given,
  else empty. Empty at the end of a chapter (the next chapter may not be laid
  out yet) and at the end of the book.
- **chapter title**: the TOC title of the current chapter, may be empty.
- **percent**: book progress, integer 0 to 100.
- **text**: the words on the page. Paragraphs are separated by `\n`; words by
  single spaces. A word hyphenated across a line break is joined back
  together. A page may begin or end mid-sentence (and mid-paragraph); when the
  previous page ended mid-paragraph, the text does not start with `\n`.
  May be empty (an image-only page).
- **next text**: the words of the following page, in the same form, or empty.

### Phone to device

| Type | Fields | Meaning |
| --- | --- | --- |
| `N` | (none) | Turn forward one page. |
| `V` | (none) | Turn back one page. |
| `R` | (none) | Send `P` for the current page again. |
| `S` | `1` or `0` | The phone is speaking (`1`) or stopped/paused (`0`). While `1`, the device does not auto-sleep. |

Unknown types are ignored in both directions.

## Phone behaviour (reference)

The phone speaks **text** followed by **next text** as one stream, split into
sentences, so a sentence crossing the page boundary is read without a pause.
When speech reaches the start of **next text**, it sends `N`; the device turns
the page and sends `P` whose key equals the previous **next key**. The phone
then already has that page's text queued and only appends the new **next
text**. A `P` with any other key (the reader turned or jumped by hand) makes
the phone drop its queue and start at the beginning of the new **text**.
When **next text** is empty, the phone sends `N` after the page is spoken and
waits for the `P` of the new page.
