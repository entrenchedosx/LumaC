-- Mover: slides its object along +X at an exported speed.
-- Demonstrates: exported properties (tunable from C), position API.
export("speed", 2.0)

function update(self, dt)
    local x, y, z = self:position()
    self:set_position(x + self.speed * dt, y, z)
end
