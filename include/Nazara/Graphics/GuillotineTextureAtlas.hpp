// Copyright (C) 2015 Jérôme Leclercq
// This file is part of the "Nazara Engine - Graphics module"
// For conditions of distribution and use, see copyright notice in Config.hpp

#pragma once

#ifndef NAZARA_GUILLOTINETEXTUREATLAS_HPP
#define NAZARA_GUILLOTINETEXTUREATLAS_HPP

#include <Nazara/Prerequesites.hpp>
#include <Nazara/Utility/GuillotineImageAtlas.hpp>

class NAZARA_API NzGuillotineTextureAtlas : public NzGuillotineImageAtlas
{
	public:
		NzGuillotineTextureAtlas() = default;
		~NzGuillotineTextureAtlas() = default;

	private:
		unsigned int GetMaxAtlasSize() const override;
		bool ResizeImage(Layer& layer, const NzVector2ui& size) const override;
};

#endif // NAZARA_GUILLOTINETEXTUREATLAS_HPP
