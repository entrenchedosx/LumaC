-- Orbiter: circles its object around the world origin.
-- Demonstrates: fixed_update() for deterministic motion, math lib.
export("radius", 4.0)
export("angular_speed", 0.8)

local angle = 0.0

function fixed_update(self, dt)
    angle = angle + self.angular_speed * dt
    self:set_position(
        self.radius * math.cos(angle),
        0.5,
        self.radius * math.sin(angle))
end
