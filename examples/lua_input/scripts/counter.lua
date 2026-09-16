-- Fixed-step counter: ticks once per fixed update.
-- Demonstrates: engine-owned fixed schedule consumed by Lua.
export("ticks", 0)

function fixed_update(self, dt)
    self.ticks = self.ticks + 1
end
