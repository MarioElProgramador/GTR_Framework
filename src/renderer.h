#pragma once
#include "prefab.h"

//forward declarations
class Camera;
class Shader;

using namespace std;

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
		bool render_shadowmap;

	public:

		enum eLightRender {
			SINGLEPASS,
			MULTIPASS
		};

		enum ePipeline {
			FORWARD,
			DEFERRED
		};
		enum eRenderShape {
			QUAD,
			GEOMETRY
		};
		enum ePipelineSpace {
			LINEAR = 0,
			GAMMA = 1
		};
		enum eDynamicRange {
			SDR = 0,
			HDR = 1
		};


		std::vector<GTR::LightEntity*> lights;
		std::vector<RenderCall> render_calls;

		eLightRender lightRender;
		ePipeline pipeline;
		eRenderShape renderShape;
		ePipelineSpace pipelineSpace;
		eDynamicRange dynamicRange;
		FBO* gbuffers_fbo;
		FBO* illumination_fbo;

		FBO* ssao_fbo;
		Texture* ssao_blur;
		bool show_gbuffers;
		bool show_ssao;

		vector<Vector3> random_points;

		Renderer();
		//add here your functions
		void renderForward(Camera* camera, GTR::Scene* scene);
		void renderDeferred(Camera* camera, GTR::Scene* scene);

		//renders several elements of the scene
		void renderScene(GTR::Scene* scene, Camera* camera);

		//to render a whole prefab (with all its nodes)
		void renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera);

		//to render one node from the prefab and its children
		void renderNode(const Matrix44& model, GTR::Node* node, Camera* camera);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterialToGBuffers(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void renderMeshWithMaterialAndLighting(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);

		void uploadLightToShaderMultipass(LightEntity* light, Shader* shader);
		void uploadLightToShaderSinglepass(Shader* shader);
		void uploadLightToShaderDeferred(Shader* shader, Matrix44 inv_vp, int width, int height, Camera* camera);

		void generateShadowmap(LightEntity* light);
		void renderFlatMesh(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera);
		void showShadowmap(LightEntity* light);
	
		void uploadUniformsAndTextures(Shader* shader, GTR::Material* material, Camera* camera, const Matrix44 model);
	};

	vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);

	Texture* CubemapFromHDRE(const char* filename);
};