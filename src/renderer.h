#pragma once
#include "prefab.h"
#include "sphericalharmonics.h"
#include "mesh.h"

//forward declarations
class Camera;
class Shader;

using namespace std;

struct sIrrHeader {
	Vector3 start;
	Vector3 end;
	Vector3 delta;
	Vector3 dims;
	int num_probes;
};

//struct to store probes
struct sProbe {
	Vector3 pos; //where is located
	Vector3 local; //its ijk pos in the matrix
	int index; //its index in the linear array
	SphericalHarmonics sh; //coeffs
};

struct sReflectionProbe {
	Vector3 pos;
	Texture* cubemap = NULL;
};

namespace GTR {

	class Prefab;
	class Material;

	// Clase para recoger información del prefab referente al render
	class RenderCall {
	public:

		Material* material;
		Mesh* mesh;
		Matrix44 model;

		BoundingBox world_bounding;
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
		std::vector<GTR::DecalEntity*> decals;
		std::vector<RenderCall> render_calls;

		LightEntity* directional;
		eLightRender lightRender;
		eRenderShape renderShape;
		ePipeline pipeline;
		ePipelineSpace pipelineSpace;
		eDynamicRange dynamicRange;

		FBO* gbuffers_fbo;
		FBO* illumination_fbo;
		FBO* ssao_fbo;
		FBO* irradiance_fbo;
		FBO* reflection_fbo;
		FBO* reflection_probe_fbo;
		FBO* decal_fbo;
		FBO* volumetric_fbo;

		Texture* ssao_blur;
		Texture* probes_texture;
		Texture* postFX_textureA;
		Texture* postFX_textureB;
		Texture* postFX_textureC;
		Texture* postFX_textureD;
		Texture* blurred_texture;

		Texture* skybox;
		Texture* cloned_depth_texture;
		Texture* blur;

		bool multilight;
		bool show_gbuffers;
		bool show_ssao;
		bool show_probes;
		bool show_reflection_probes;
		bool show_probes_texture;
		bool show_irradiance;
		bool show_reflections;
		bool show_decal;
		bool is_rendering_reflections;
		bool show_chrab_lensdist;
		bool show_motblur;
		bool show_antial;
		bool show_DoF;
		bool show_volumetric;

		vector<Vector3> random_points;
		Vector3 start_irr;
		Vector3 end_irr;
		Vector3 dim_irr;
		Vector3 delta;
		// sProbe probe;
		vector<sProbe> probes;
		vector<sReflectionProbe*> reflection_probes;

		Mesh cube;
		Matrix44 viewproj_old;
		float deb_fac;
		float minDist;
		float maxDist;
		float air_density;
		float contrast;
		float intensity_factor;
		float threshold;

		Renderer();

		//add here your functions

		//renders diferent types
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

		void generateSkybox(Camera* camera);

		//to render probes 
		void generateProbe(GTR::Scene* scene);
		void renderProbe(Vector3 pos, float size, float* coeffs);
		void captureProbe(sProbe& probe, GTR::Scene* scene);
		bool loadProbes();
		void uploadProbesToGPU();

		void renderReflectionProbes(GTR::Scene* scene, Camera* camera);
		void updateReflectionProbes(GTR::Scene* scene);
		void captureReflectionProbe(GTR::Scene* scene, Texture* tex, Vector3 pos);

		void uploadUniformsAndTextures(Shader* shader, GTR::Material* material, Camera* camera, const Matrix44 model);
		void applyfx(Texture* color, Texture* depth, Camera* camera);
	};

	vector<Vector3> generateSpherePoints(int num, float radius, bool hemi);

	Texture* CubemapFromHDRE(const char* filename);
};