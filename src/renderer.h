#pragma once
#include "prefab.h"

//forward declarations
class Camera;

namespace GTR {

	class Prefab;
	class Material;

	// Clase para recoger información del prefab referente al render
	class RenderCall {
	public:
		Material* material;
		Mesh* mesh;
		BoundingBox world_bounding;
		Matrix44 model;

		float distance_to_camera = 0.0;

		bool operator > (const RenderCall& str) const
		{
			if (material->alpha_mode == eAlphaMode::BLEND)
				if (str.material->alpha_mode == eAlphaMode::BLEND)
					return (distance_to_camera > str.distance_to_camera);
				else
					return false;
			else
				if (str.material->alpha_mode == eAlphaMode::BLEND)
					return true;
				else
					return (distance_to_camera > str.distance_to_camera);
		}
	};

	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{

	public:

		std::vector<GTR::LightEntity*> lights;
		std::vector<RenderCall> render_calls;

		//add here your functions

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);

		//to render a whole prefab (with all its nodes)
		void renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void renderNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);

		void generateShadowmap(LightEntity* light);
		void renderFlatMesh(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void showShadowmap(LightEntity* light);
	};

	Texture* CubemapFromHDRE(const char* filename);
};