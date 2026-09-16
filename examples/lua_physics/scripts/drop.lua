-- Falling crate: reports its first ground contact, then rides out
-- the bounce. Gravity + restitution come from the engine (set up
-- in main.c); this script only observes.
local announced = false

function collision_enter(self, other, contact)
  if announced then return end
  announced = true
  local name = other:name()
  if name == "" then name = "(unnamed)" end
  print(string.format(
    "[drop] first contact with %s (penetration %.3f m)",
    name, contact.penetration))
end

function update(self, dt)
  if announced then return end
  local x, y, z = self:position()
  if y < 1.2 then
    print(string.format("[drop] passing y=%.2f", y))
  end
end
