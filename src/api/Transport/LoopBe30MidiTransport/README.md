# LoopBe30 Windows MIDI Services transport

`Midi2.LoopBe30MidiTransport.dll` is the LoopBe30 2.0 local loopback transport. It
publishes up to 30 persistent bidirectional MIDI 1.0 UMP endpoints with the
fixed identities in `loopbe30_transport_defs.h`. It does not depend on the
Microsoft Basic Loopback transport or the Windows MIDI Services SDK runtime.

Messages are validated and admitted to fixed per-port queues as complete
batches. A fair worker delivers them asynchronously, so the sender is never
called back inline and no `WaitForSendComplete` state crosses the transport.
The worker also owns the feedback detector and latches per-port feedback,
queue-overload, and delivery-failure safety mutes.

Machine configuration is stored below
`HKLM\SOFTWARE\nerds.de\LoopBe30\Parameters`. Only manual mutes are persisted;
safety mutes clear when the MIDI service restarts. The `Trial` build starts its
protected volatile 60-minute timer when the first port is opened.
