-- Camera mouse-look: yaw the rig from accumulated mouse delta.
-- Demonstrates: Input.mouse_delta(), exported sensitivity.
export("sensitivity", 0.003)

function update(self, dt)
    local dx, dy = Input.mouse_delta()
    if dx ~= 0 then
        self:rotate_y(-dx * self.sensitivity)
    end
end
