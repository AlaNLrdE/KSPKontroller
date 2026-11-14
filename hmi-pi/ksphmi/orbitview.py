"""Body-centred inertial orbit geometry, independent of attitude and periapsis angle conventions."""
from dataclasses import dataclass
import math
from .navigation import dot, unit


def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0])


def view_axes(position, velocity):
    """Choose a fixed inertial view plane once per vessel/body, never from the eccentricity vector."""
    normal = unit(cross(position, velocity))
    if normal is None:
        return ((1, 0, 0), (0, 0, 1))
    for reference in ((1, 0, 0), (0, 0, 1)):
        right = unit(tuple(a - dot(reference, normal) * b for a, b in zip(reference, normal)))
        if right is not None:
            return right, cross(normal, right)


@dataclass(frozen=True)
class OrbitState:
    position: tuple
    velocity: tuple
    mu: float
    axes: tuple

    def project(self, vector):
        return dot(vector, self.axes[0]), dot(vector, self.axes[1])

    @property
    def vessel_point(self):
        # The marker is always the measured Cartesian position. Do not reconstruct
        # it from true anomaly: periapsis is ill-conditioned in circular orbits.
        return self.project(self.position)

    def conic(self):
        radius = math.sqrt(dot(self.position, self.position))
        if radius <= 0 or self.mu <= 0 or not all(math.isfinite(v) for v in (*self.position, *self.velocity, self.mu)):
            return None
        h = cross(self.position, self.velocity)
        normal = unit(h)
        if normal is None:
            return None
        speed2, rv = dot(self.velocity, self.velocity), dot(self.position, self.velocity)
        eccentricity_vector = tuple(((speed2 - self.mu / radius) * r - rv * v) / self.mu
                                    for r, v in zip(self.position, self.velocity))
        eccentricity = math.sqrt(dot(eccentricity_vector, eccentricity_vector))
        if eccentricity > 1e-8:
            peri_direction = unit(eccentricity_vector)
        else:
            # A circle has no unique periapsis. Choose a stable drawing basis.
            right = self.axes[0]
            peri_direction = unit(tuple(a - dot(right, normal) * b for a, b in zip(right, normal)))
            if peri_direction is None:
                peri_direction = unit(self.position)
        transverse = cross(normal, peri_direction)
        return Conic(dot(h, h) / self.mu, eccentricity, peri_direction, transverse)


@dataclass(frozen=True)
class Conic:
    p: float
    eccentricity: float
    peri_direction: tuple
    transverse: tuple

    @property
    def periapsis(self):
        return self.p / (1 + self.eccentricity)

    @property
    def apoapsis(self):
        return self.p / (1 - self.eccentricity) if self.eccentricity < 1 else None

    def position(self, anomaly):
        denominator = 1 + self.eccentricity * math.cos(anomaly)
        if denominator <= 1e-6:
            return None
        radius = self.p / denominator
        return tuple(radius * (math.cos(anomaly) * p + math.sin(anomaly) * q)
                     for p, q in zip(self.peri_direction, self.transverse))
