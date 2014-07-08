// Copyright (C) 2014 Jérôme Leclercq
// This file is part of the "Nazara Engine - Mathematics module"
// For conditions of distribution and use, see copyright notice in Config.hpp

#include <Nazara/Core/StringStream.hpp>
#include <Nazara/Core/Debug.hpp>

#define F(a) static_cast<T>(a)

template<typename T>
NzRay<T>::NzRay(T X, T Y, T Z, T DirectionX, T DirectionY, T DirectionZ)
{
    Set(X, Y, Z, DirectionX, DirectionY, DirectionZ);
}

template<typename T>
NzRay<T>::NzRay(const T Origin[3], const T Direction[3])
{
    Set(Origin, Direction);
}

template<typename T>
NzRay<T>::NzRay(const NzVector3<T>& Origin, const NzVector3<T>& Direction)
{
    Set(Origin, Direction);
}

template<typename T>
NzRay<T>::NzRay(const NzPlane<T>& planeOne, const NzPlane<T>& planeTwo)
{
    Set(planeOne, planeTwo);
}

template<typename T>
template<typename U>
NzRay<T>::NzRay(const NzVector3<U>& Origin, const NzVector3<U>& Direction)
{
	Set(Origin, Direction);
}

template<typename T>
template<typename U>
NzRay<T>::NzRay(const NzRay<U>& ray)
{
	Set(ray);
}

template<typename T>
NzVector3<T> NzRay<T>::GetClosestPoint(const NzVector3<T>& point) const
{
    NzVector3<T> delta = point - origin;
    T vsq = direction.GetSquaredLength();
    T proj = delta.DotProduct(direction);

    return GetPoint(proj/vsq);
}

template<typename T>
NzVector3<T> NzRay<T>::GetPoint(T lambda) const
{
    return NzVector3<T>(origin + direction * lambda);
}

template<typename T>
bool NzRay<T>::Intersect(const NzBox<T>& box, NzVector3<T> * hitPoint, NzVector3<T> * hitSecondPoint) const
{
    // Slab method

	#if NAZARA_MATH_SAFE
	if (NzNumberEquals(direction.x, F(0.0)) || NzNumberEquals(direction.y, F(0.0)) || NzNumberEquals(direction.z, F(0.0)))
	{
		NazaraWarning("Division by zero !"); // The algorithm is still correct.
	}
	#endif

    T tx1 = (box.x - origin.x) / direction.x;
    T tx2 = (box.x + box.width - origin.x) / direction.x;

    T tmin = std::min(tx1, tx2);
    T tmax = std::max(tx1, tx2);

    T ty1 = (box.y - origin.y) / direction.y;
    T ty2 = (box.y + box.height - origin.y) / direction.y;

    tmin = std::max(tmin, std::min(ty1, ty2));
    tmax = std::min(tmax, std::max(ty1, ty2));

    T tz1 = (box.z - origin.z) / direction.z;
    T tz2 = (box.z + box.depth - origin.z) / direction.z;

    tmin = std::max(tmin, std::min(tz1, tz2));
    tmax = std::min(tmax, std::max(tz1, tz2));

    if (hitPoint)
        hitPoint->Set(GetPoint(tmin));
    if (hitSecondPoint)
        hitSecondPoint->Set(GetPoint(tmax));

    return tmax >= std::max(F(0.0), tmin) && tmin < INFINITY;
}

template<typename T>
bool NzRay<T>::Intersect(const NzOrientedBox<T>& orientedBox, NzVector3<T> * hitPoint, NzVector3<T> * hitSecondPoint) const
{
    NzVector3<T> width = (orientedBox.GetCorner(nzCorner_NearLeftBottom) - orientedBox.GetCorner(nzCorner_FarLeftBottom)).Normalize();
    NzVector3<T> height = (orientedBox.GetCorner(nzCorner_FarLeftTop) - orientedBox.GetCorner(nzCorner_FarLeftBottom)).Normalize();
    NzVector3<T> depth = (orientedBox.GetCorner(nzCorner_FarRightBottom) - orientedBox.GetCorner(nzCorner_FarLeftBottom)).Normalize();

    // Construction of the inverse of the matrix who did the rotation -> orthogonal matrix.
    NzMatrix4<T> transformation(width.x, height.x, depth.x, F(0.0),
                                width.y, height.y, depth.y, F(0.0),
                                width.z, height.z, depth.z, F(0.0),
                                F(0.0),  F(0.0),   F(0.0),  F(1.0));

    // Reduction to aabb problem
    NzVector3<T> newOrigin = transformation.Transform(origin);
    NzVector3<T> newDirection = transformation.Transform(direction);

    NzVector3<T> tmp, tmp2;
    if (NzRay<T>(newOrigin, newDirection).Intersect(NzBox<T>(orientedBox.GetCorner(nzCorner_NearRightTop), orientedBox.GetCorner(nzCorner_FarLeftBottom)), &tmp, &tmp2))
    {
        if (hitPoint)
        {
            transformation.Transpose();
            hitPoint->Set(transformation.Transform(tmp));
            if (hitSecondPoint)
                hitSecondPoint->Set(transformation.Transform(tmp2));
        }

        return true;
    }

	return false;
}

template<typename T>
bool NzRay<T>::Intersect(const NzPlane<T>& plane, NzVector3<T> * hitPoint) const
{
    T divisor = plane.normal.DotProduct(direction);

    if (NzNumberEquals(divisor, F(0.0)))
        return false; // perpendicular

    if (!hitPoint)
        return true;

    T lambda = - (plane.normal.DotProduct(origin) - plane.distance) / divisor; // The plane is ax+by+cz=d
    hitPoint->Set(GetPoint(lambda));

    return true;
}

template<typename T>
bool NzRay<T>::Intersect(const NzSphere<T>& sphere, NzVector3<T> * hitPoint, NzVector3<T> * hitSecondPoint) const
{
    NzVector3<T> distanceCenterOrigin = sphere.GetPosition() - origin;
    T length = distanceCenterOrigin.DotProduct(direction);

    if (length < F(0.0))
        return false; // ray is perpendicular to the vector origin - center

    T squaredDistance = distanceCenterOrigin.GetSquaredLength() - length * length;

    T squaredRadius = sphere.GetRadius() * sphere.GetRadius();

    if (squaredDistance > squaredRadius)
        return false; // if the ray is further than the radius

    if (!hitPoint)
        return true;

    T deltaLambda = std::sqrt(squaredRadius - squaredDistance);

    if (hitPoint)
        hitPoint->Set(GetPoint(length - deltaLambda));
    if (hitSecondPoint)
        hitSecondPoint->Set(GetPoint(length + deltaLambda));

    return true;
}

template<typename T>
NzVector3<T> NzRay<T>::operator*(T lambda) const
{
    return GetPoint(lambda);
}

template<typename T>
NzRay<T>& NzRay<T>::Set(T X, T Y, T Z, T directionX, T directionY, T directionZ)
{
    direction = NzVector3<T>(directionX, directionY, directionZ);
    origin = NzVector3<T>(X, Y, Z);

    return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::Set(const T Origin[3], const T Direction[3])
{
    direction = NzVector3<T>(Direction);
    origin = NzVector3<T>(Origin);

    return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::Set(const NzVector3<T>& Origin, const NzVector3<T>& Direction)
{
    direction = Direction;
    origin = Origin;

    return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::Set(const NzPlane<T>& planeOne, const NzPlane<T>& planeTwo)
{
    T termOne = planeOne.normal.GetLength();
    T termTwo = planeOne.normal.DotProduct(planeTwo.normal);
    T termFour = planeTwo.normal.GetLength();
    T det = termOne * termFour - termTwo * termTwo;

    #if NAZARA_MATH_SAFE
    if (NzNumberEquals(det, F(0.0)))
    {

        NzString error("Planes are parallel.");

        NazaraError(error);
        throw std::domain_error(error);
    }
    #endif

    T invdet = F(1.0) / det;
    T fc0 = (termFour * -planeOne.distance + termTwo * planeTwo.distance) * invdet;
    T fc1 = (termOne * -planeTwo.distance + termTwo * planeOne.distance) * invdet;

    direction = planeOne.normal.CrossProduct(planeTwo.normal);
    origin = planeOne.normal * fc0 + planeTwo.normal * fc1;

    return *this;
}

template<typename T>
template<typename U>
NzRay<T>& NzRay<T>::Set(const NzVector3<U>& Origin, const NzVector3<U>& Direction)
{
    direction = NzVector3<T>(Direction);
    origin = NzVector3<T>(Origin);

    return *this;
}

template<typename T>
template<typename U>
NzRay<T>& NzRay<T>::Set(const NzRay<U>& ray)
{
    direction = NzVector3<T>(ray.direction);
    origin = NzVector3<T>(ray.origin);

    return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::Set(const NzRay& ray)
{
	std::memcpy(this, &ray, sizeof(NzRay));

	return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::SetDirection(const NzVector3<T>& Direction)
{
    direction = Direction;

    return *this;
}

template<typename T>
NzRay<T>& NzRay<T>::SetOrigin(const NzVector3<T>& Origin)
{
    origin = Origin;

    return *this;
}

template<typename T>
NzString NzRay<T>::ToString() const
{
	NzStringStream ss;

	return ss << "Ray(origin: " << origin.ToString() << ", direction: " << direction.ToString() << ")";
}

template<typename T>
NzRay<T> NzRay<T>::Lerp(const NzRay& from, const NzRay& to, T interpolation)
{
	return NzRay<T>(from.origin.Lerp(to.origin, interpolation), from.direction.Lerp(to.direction, interpolation));
}

template<typename T>
NzRay<T> NzRay<T>::UnitX()
{
    return NzRay(NzVector3<T>::Zero(), NzVector3<T>::UnitX());
}

template<typename T>
NzRay<T> NzRay<T>::UnitY()
{
    return NzRay(NzVector3<T>::Zero(), NzVector3<T>::UnitY());
}

template<typename T>
NzRay<T> NzRay<T>::UnitZ()
{
    return NzRay(NzVector3<T>::Zero(), NzVector3<T>::UnitZ());
}

template<typename T>
std::ostream& operator<<(std::ostream& out, const NzRay<T>& ray)
{
	return out << ray.ToString();
}

#undef F

#include <Nazara/Core/DebugOff.hpp>