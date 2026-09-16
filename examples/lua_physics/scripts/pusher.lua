-- Kinematic pusher: patrols along X on the fixed-step clock and
-- shoves dynamics out of the way (infinite mass vs finite mass).
-- The engine derives its contact velocity from transform motion.
export("speed", 2.0)
export("range", 3.0)

local t = 0.0

function fixed_update(self, dt)
  t = t + dt
  local x = self.range * math.sin(t * self.speed * 0.5)
  local _, y, z = self:position()
  self:set_position(x, y, z)
end

function collision_enter(self, other, contact)
  local name = other:name()
  if name == "" then name = "(unnamed)" end
  print(string.format("[pusher] shoving %s", name))
end
