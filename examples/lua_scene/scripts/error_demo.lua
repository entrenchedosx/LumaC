-- Error demo: calls an undefined global on its third update.
-- Demonstrates: the error policy — this instance disables itself
-- (failed=1) while the rest of the world keeps running.
local ticks = 0

function update(self, dt)
    ticks = ticks + 1
    if ticks == 3 then
        boom() -- runtime error: attempt to call a nil value
    end
end
