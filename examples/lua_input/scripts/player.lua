-- Player: WASD via axes, Space hop via the jump action,
-- Escape pause toggle via the pause action + Time scale.
export("speed", 6.0)

function update(self, dt)
    local mx = Input.axis("move_x")
    local mz = Input.axis("move_z")
    if mx ~= 0 or mz ~= 0 then
        local x, y, z = self:position()
        self:set_position(x + mx * self.speed * dt, y,
                          z + mz * self.speed * dt)
    end
    if Input.action_pressed("jump") then
        local x, y, z = self:position()
        self:set_position(x, y + 1.0, z)
    end
    if Input.action_pressed("pause") then
        if Time.scale() == 0 then
            Time.set_scale(1)
        else
            Time.set_scale(0)
        end
    end
end
