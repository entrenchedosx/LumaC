# Command Recording Architecture

Phase 20 separates mutable recording contexts from immutable recorded work.
`lc_command_encoder_create` creates a worker encoder and
`lc_command_encoder_finish` returns a single-use `lc_command_list`. Applications
execute an ordered list batch inside an open offscreen pass. Vulkan handles
never cross the public API.

Each worker owns one Vulkan command pool. One CPU thread may use an encoder at a
time; different encoders need no recording mutex and never synchronize the same
pool. Finished lists use secondary command buffers and carry their own dynamic
viewport/scissor state. Buffers return to their pool when a list is destroyed;
a destroyed encoder becomes a zombie owner until its last list is released.
Destroying an already-submitted list invalidates the public handle immediately
but defers its Vulkan command-buffer and pool release until the frame timeline
value completes. Device shutdown drains any abandoned encoders and lists after
the shutdown-only idle wait.

Workers never mutate global image state while recording. A list stores state
intents and required input states. Batch execution validates list identity,
target compatibility, referenced-object liveness, and state requirements in
caller-provided order before emitting `vkCmdExecuteCommands`. It advances state
only for that accepted execution order. Recording chronology has no execution
meaning; an incompatible submission order fails loudly.

The initial offscreen pass stays inline-capable. Batch execution ends that
instance and reopens a render-pass-compatible LOAD instance with secondary
contents, preserving earlier inline output and Vulkan's contents rules.

Lists are single-use; graphics lists execute inside one offscreen
pass, compute lists (dispatch/buffer-transition/push, no
render-pass inheritance) execute outside passes, never mixed in
one batch. Indirect draws record in graphics lists and on frame
encoders. Render-graph scheduling remains a future layer over
the same encoder/list boundary.
