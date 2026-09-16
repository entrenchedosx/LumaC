-- Spinner: rotates its object around Y every frame.
-- Demonstrates: self as arg + global, engine-driven update(dt).
export("speed", 1.5)

function start(self)
end

function update(self, dt)
    self:rotate_y(self.speed * dt)
end

function destroy(self)
end
