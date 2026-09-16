-- Trigger zone: counts overlapping bodies without touching them
-- (triggers never enter the solver). EXIT carries a nil contact.
local inside = 0

function trigger_enter(self, other, contact)
  inside = inside + 1
  local name = other:name()
  if name == "" then name = "(unnamed)" end
  print(string.format("[zone] %s entered (%d inside)", name,
                      inside))
end

function trigger_exit(self, other, contact)
  inside = inside - 1
  if inside < 0 then inside = 0 end
  local name = other:name()
  if name == "" then name = "(unnamed)" end
  print(string.format("[zone] %s left (%d inside)", name, inside))
end
