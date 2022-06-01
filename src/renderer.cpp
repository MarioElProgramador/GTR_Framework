#include "renderer.h"

#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "texture.h"
#include "prefab.h"
#include "material.h"
#include "utils.h"
#include "scene.h"
#include "extra/hdre.h"

#include "fbo.h"
#include "application.h"

#include <algorithm>    // Sorting algorithm

using namespace GTR;

GTR::Renderer::Renderer() {
	lightRender = MULTIPASS;
	pipeline = DEFERRED;
	renderShape = GEOMETRY;
	pipelineSpace = GAMMA;
	dynamicRange = HDR;
	gbuffers_fbo = NULL;
	illumination_fbo = NULL;
	ssao_fbo = NULL;
	ssao_blur = NULL;
	show_gbuffers = false;
	show_ssao = false;
	random_points = generateSpherePoints(64, 1, true);
}

vector<Vector3> GTR::generateSpherePoints(int num, float radius, bool hemi) {
	vector<Vector3> points;
	points.resize(num);
	for (int i = 0; i < num; i++) {
		Vector3& p = points[i];
		float u = random();
		float v = random();
		float theta = u * 2.0 * PI;
		float phi = acos(2.0 * v - 1.0);
		float r = cbrt(random() * 0.9 + 0.1) * radius;
		float sinTheta = sin(theta);
		float cosTheta = cos(theta);
		float sinPhi = sin(phi);
		float cosPhi = cos(phi);
		p.x = r * sinPhi * cosTheta;
		p.y = r * sinPhi * sinTheta;
		p.z = r * cosPhi;
		if (hemi && p.z < 0)
			p.z *= -1.0;
	}
	return points;
}

void GTR::Renderer::renderForward(Camera* camera, GTR::Scene* scene) {
	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();
	for (vector<GTR::RenderCall>::iterator rc = render_calls.begin(); rc != render_calls.end(); ++rc) {
		if (camera->testBoxInFrustum(rc->world_bounding.center, rc->world_bounding.halfsize))
			renderMeshWithMaterialAndLighting(rc->model, rc->mesh, rc->material, camera);
	}
}

void GTR::Renderer::renderDeferred(Camera* camera, GTR::Scene* scene) {
	//Render GBuffers
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;

	//Crear GBuffer si no existen
	if (!gbuffers_fbo) {
		gbuffers_fbo = new FBO();

		//create 3 textures of 4 components
		gbuffers_fbo->create(width, height,
			3, 			//three textures
			GL_RGBA, 		//four channels
			GL_UNSIGNED_BYTE, //1 byte
			true);		//add depth_texture
	}
	if (!illumination_fbo) {
		illumination_fbo = new FBO();

		illumination_fbo->create(width, height,
			1,			//one texture
			GL_RGB,			//three channels
			GL_FLOAT,	//1 byte
			true);		//add depth_texture
	}
	if (!ssao_fbo) {
		ssao_fbo = new FBO();
		ssao_fbo->create(width, height,
			1,			//one texture
			GL_RGB,			//three channels
			GL_UNSIGNED_BYTE,	//1 byte
			false);		//add depth_texture

		ssao_blur = new Texture();
		ssao_blur->create(width, height);
	}

	Mesh* quad = Mesh::getQuad();
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false, false);
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	gbuffers_fbo->bind();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	checkGLErrors();

	//Renderizar cada objeto con un GBuffer shader
	for (vector<GTR::RenderCall>::iterator rc = render_calls.begin(); rc != render_calls.end(); ++rc) {
		if (camera->testBoxInFrustum(rc->world_bounding.center, rc->world_bounding.halfsize))
			renderMeshWithMaterialToGBuffers(rc->model, rc->mesh, rc->material, camera);
	}

	gbuffers_fbo->unbind();

	ssao_fbo->bind();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	Shader* shader_ssao = Shader::Get("ssao");
	shader_ssao->enable();
	shader_ssao->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader_ssao->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader_ssao->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 3);
	shader_ssao->setUniform("u_inverse_viewprojection", inv_vp);
	shader_ssao->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	shader_ssao->setUniform3Array("u_points", (float*)&random_points[0], random_points.size());

	quad->render(GL_TRIANGLES);

	Shader* shader_blur = Shader::Get("ssao_blur");
	shader_blur->enable();
	shader_blur->setUniform("ssaoInput", ssao_fbo->color_textures[0], 1);
	quad->render(GL_TRIANGLES);

	ssao_fbo->unbind();

	illumination_fbo->bind();

	gbuffers_fbo->depth_texture->copyTo(NULL);
	glClear(GL_COLOR_BUFFER_BIT);

	glDisable(GL_DEPTH_TEST);

	//we need a fullscreen quad
	Shader* shader = Shader::Get("deferred");

	shader->enable();
	shader->setUniform("u_ambient_light", scene->ambient_light);

	uploadLightToShaderDeferred(shader, inv_vp, width, height, camera);

	int num_lights = lights.size();

	// Multipass
	for (int i = 0; i < num_lights; i++) {
		LightEntity* light = lights[i];
		if (light->light_type == DIRECTIONAL) { // DIRECTIONAL light
			uploadLightToShaderMultipass(light, shader);
		}
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	quad->render(GL_TRIANGLES);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	if (!num_lights) {
		shader->setUniform("u_light_color", Vector3());
		quad->render(GL_TRIANGLES);
	}
	else {
		for (int i = 0; i < num_lights; ++i) {
			LightEntity* light = lights[i];

			Matrix44 m;
			Vector3 lightpos = light->model.getTranslation();
			m.setTranslation(lightpos.x, lightpos.y, lightpos.z);
			m.scale(light->max_distance, light->max_distance, light->max_distance);

			if (renderShape == GEOMETRY && light->light_type != DIRECTIONAL) {
				shader = Shader::Get("sphere_deferred");

				shader->enable();

				uploadLightToShaderMultipass(light, shader);
				uploadLightToShaderDeferred(shader, inv_vp, width, height, camera);

				shader->setUniform("u_model", m);
				shader->setUniform("u_viewprojection", camera->viewprojection_matrix);

				glEnable(GL_CULL_FACE);

				//render only the backfacing triangles of the sphere
				glFrontFace(GL_CW);

				//and render the sphere
				sphere->render(GL_TRIANGLES);
			}
			else if (renderShape == QUAD) {
				glDisable(GL_CULL_FACE);
				glFrontFace(GL_CCW);

				shader = Shader::Get("deferred");

				shader->enable();

				uploadLightToShaderMultipass(light, shader);
				uploadLightToShaderDeferred(shader, inv_vp, width, height, camera);

				//do the draw call that renders the mesh into the screen
				quad->render(GL_TRIANGLES);
			}
			shader->setUniform("u_ambient_light", Vector3());
			shader->setUniform("u_emissive_factor", Vector3());
		}
	}
	glDisable(GL_CULL_FACE);
	glFrontFace(GL_CCW);

	// To enable the z-buffer so the grid does not appear over the objects
	glEnable(GL_DEPTH_TEST);

	// Render alphanodes in forward mode
	for (vector<GTR::RenderCall>::iterator rc = render_calls.begin(); rc != render_calls.end(); ++rc) {
		if (camera->testBoxInFrustum(rc->world_bounding.center, rc->world_bounding.halfsize) && rc->material->alpha_mode == GTR::eAlphaMode::BLEND) {
			renderMeshWithMaterialAndLighting(rc->model, rc->mesh, rc->material, camera);
		}
	}

	illumination_fbo->unbind();
	glDisable(GL_BLEND);
	illumination_fbo->color_textures[0]->toViewport();

	if (show_gbuffers) {
		glViewport(0, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[0]->toViewport();
		glViewport(width * 0.5, height * 0.5, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[1]->toViewport();
		glViewport(0, 0, width * 0.5, height * 0.5);
		gbuffers_fbo->color_textures[2]->toViewport();
		glViewport(width * 0.5, 0, width * 0.5, height * 0.5);

		Shader* shader = Shader::getDefaultShader("depth");
		shader->enable();
		shader->setUniform("u_camera_nearfar", Vector2(camera->near_plane, camera->far_plane));

		gbuffers_fbo->depth_texture->toViewport(shader);
		glViewport(0, 0, width, height);
	}

	if (show_ssao) {
		ssao_fbo->color_textures[0]->toViewport();
	}
}

void Renderer::renderScene(GTR::Scene* scene, Camera* camera)
{
	lights.clear();
	render_calls.clear();

	//rendering entities
	for (int i = 0; i < scene->entities.size(); ++i) {
		BaseEntity* ent = scene->entities[i];
		if (!ent->visible)
			continue;

		//is a light!
		if (ent->entity_type == LIGHT)
		{
			LightEntity* light = (GTR::LightEntity*)ent;
			lights.push_back(light);
		}

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab) {
				renderPrefab(ent->model, pent->prefab, camera);
			}
		}
	}

	//shadowmaps
	for (int i = 0; i < lights.size(); i++) {
		LightEntity* light = lights[i];
		if (light->cast_shadows)
			generateShadowmap(light);
	}

	//rendercalls
	sort(render_calls.begin(), render_calls.end(), std::greater<RenderCall>());
	if (pipeline == FORWARD)
		renderForward(camera, scene);
	else if (pipeline == DEFERRED)
		renderDeferred(camera, scene);
}

//renders all the prefab
void Renderer::renderPrefab(const Matrix44& model, GTR::Prefab* prefab, Camera* camera)
{
	assert(prefab && "PREFAB IS NULL");
	//assign the model to the root node
	renderNode(model, &prefab->root, camera);
}

//renders a node of the prefab and its children
void Renderer::renderNode(const Matrix44& prefab_model, GTR::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true) * prefab_model;

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model, node->mesh->box);

		RenderCall rc;
		Vector3 nodepos = node_model.getTranslation();
		rc.material = node->material;
		rc.model = node_model;
		rc.mesh = node->mesh;
		rc.world_bounding = world_bounding;
		rc.distance_to_camera = nodepos.distance(camera->eye);
		if (node->material->alpha_mode == GTR::eAlphaMode::BLEND)
			rc.distance_to_camera += INFINITE;
		render_calls.push_back(rc);
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		renderNode(prefab_model, node->children[i], camera);
}

//renders a mesh given its transform and material
void GTR::Renderer::renderMeshWithMaterialToGBuffers(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera) {
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	if (material->alpha_mode == eAlphaMode::BLEND)
		return;

	//define locals to simplify coding
	Shader* shader = NULL;


	//select the blending
	if (material->alpha_mode == GTR::eAlphaMode::BLEND) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDisable(GL_BLEND);
	}

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	shader = Shader::Get("gbuffers");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	uploadUniformsAndTextures(shader, material, camera, model);

	//Gamma mode
	shader->setUniform("gamma_mode", (int)pipelineSpace);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterialAndLighting(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	//select the blending
	if (material->alpha_mode == GTR::eAlphaMode::BLEND) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glDisable(GL_BLEND);
	}

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	if (lightRender == MULTIPASS) shader = Shader::Get("multilight");
	else shader = Shader::Get("singlelight");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	uploadUniformsAndTextures(shader, material, camera, model);

	//Light
	shader->setUniform("u_ambient_light", scene->ambient_light);
	glDepthFunc(GL_LEQUAL);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);

	int num_lights = lights.size();

	// Multilight
	if (!num_lights) {
		shader->setUniform("u_light_color", Vector3());
		mesh->render(GL_TRIANGLES);
	}
	else if (lightRender == MULTIPASS) {
		for (int i = 0; i < num_lights; ++i) {
			if (i == 0) {
				glDisable(GL_BLEND);
				if (material->alpha_mode == GTR::eAlphaMode::BLEND)
				{
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				}
				else {
					glDisable(GL_BLEND);
				}
			}
			else {
				glBlendFunc(GL_SRC_ALPHA, GL_ONE); 

				glEnable(GL_BLEND);
			}
			LightEntity* light = lights[i];

			uploadLightToShaderMultipass(light, shader);

			//do the draw call that renders the mesh into the screen
			mesh->render(GL_TRIANGLES);

			shader->setUniform("u_ambient_light", Vector3());
			shader->setUniform("u_emissive_factor", Vector3());
		}
	}

	// Singlelight
	else {
		uploadLightToShaderSinglepass(shader);

		//do the draw call that renders the mesh into the screen
		mesh->render(GL_TRIANGLES);

		shader->setUniform("u_ambient_light", Vector3());
		shader->setUniform("u_emissive_factor", Vector3());
	}

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glDepthFunc(GL_LESS);
}

Texture* GTR::CubemapFromHDRE(const char* filename)
{
	HDRE* hdre = HDRE::Get(filename);
	if (!hdre)
		return NULL;

	Texture* texture = new Texture();
	if (hdre->getFacesf(0))
	{
		texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesf(0),
			hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_FLOAT);
		for (int i = 1; i < hdre->levels; ++i)
			texture->uploadCubemap(texture->format, texture->type, false,
				(Uint8**)hdre->getFacesf(i), GL_RGBA32F, i);
	}
	else
		if (hdre->getFacesh(0))
		{
			texture->createCubemap(hdre->width, hdre->height, (Uint8**)hdre->getFacesh(0),
				hdre->header.numChannels == 3 ? GL_RGB : GL_RGBA, GL_HALF_FLOAT);
			for (int i = 1; i < hdre->levels; ++i)
				texture->uploadCubemap(texture->format, texture->type, false,
					(Uint8**)hdre->getFacesh(i), GL_RGBA16F, i);
		}
	return texture;
}

void Renderer::renderFlatMesh(const Matrix44 model, Mesh* mesh, GTR::Material* material, Camera* camera)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	Shader* shader = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	//chose a shader
	shader = Shader::Get("flat");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_model", model);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);

	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();
}

void GTR::Renderer::generateShadowmap(LightEntity* light) {
	if (!light->cast_shadows) {
		if (light->fbo) {
			delete light->fbo;
			light->fbo = NULL;
			light->shadowmap = NULL;
		}
		return;
	}

	if (!light->fbo) {
		light->fbo = new FBO();
		light->fbo->setDepthOnly(1024, 1024);
		light->shadowmap = light->fbo->depth_texture;
	}

	if (!light->light_camera)
		light->light_camera = new Camera();

	light->fbo->bind();

	Camera* light_camera = light->light_camera;
	Camera* view_camera = Camera::current;

	float aspect = 1.0;

	if (light->light_type == eLightType::SPOT) {
		light_camera->setPerspective(light->cone_angle * 2, aspect, 0.1, light->max_distance);
		light_camera->lookAt(light->model.getTranslation(), light->model * Vector3(0, 0, -1), light->model.rotateVector(Vector3(0, 1, 0)));
		light_camera->enable();
	}
	else if (light->light_type == eLightType::DIRECTIONAL) {
		//setup view
		Vector3 up(0, 1, 0);
		light_camera->lookAt(light->model * Vector3(), light->model * Vector3() + light->model.rotateVector(Vector3(0, 0, 1)), up);

		//use light area to define how big the frustum is
		float halfarea = light->area_size / 2;
		light_camera->setOrthographic(-halfarea, halfarea, halfarea * aspect, -halfarea * aspect, 0.1, light->max_distance);

		//compute texel size in world units, where frustum size is the distance from left to right in the camera
		float frustum_size = view_camera->left - view_camera->right;
		float grid = frustum_size / (float)light->fbo->depth_texture->width;

		//snap camera X,Y to that size in camera space assuming	the frustum is square, otherwise compute gridxand gridy
		light_camera->view_matrix.M[3][0] = round(light_camera->view_matrix.M[3][0] / grid) * grid;

		light_camera->view_matrix.M[3][1] = round(light_camera->view_matrix.M[3][1] / grid) * grid;

		//update viewproj matrix (be sure no one changes it)
		light_camera->viewprojection_matrix = light_camera->view_matrix * light_camera->projection_matrix;

		light_camera->enable();
	}

	glClear(GL_DEPTH_BUFFER_BIT);

	for (int i = 0; i < render_calls.size(); i++) {
		RenderCall& rc = render_calls[i];
		if (rc.material->alpha_mode == eAlphaMode::BLEND)
			continue;
		if (light_camera->testBoxInFrustum(rc.world_bounding.center, rc.world_bounding.halfsize))
			renderFlatMesh(rc.model, rc.mesh, rc.material, light_camera);
	}

	light->fbo->unbind();
	view_camera->enable();
}

// Shader Multipass
void GTR::Renderer::uploadLightToShaderMultipass(LightEntity* light, Shader* shader) {
	shader->setUniform("u_light_color", light->color * light->intensity);
	shader->setUniform("u_light_position", light->model * Vector3());
	shader->setUniform("u_light_max_distance", light->max_distance);
	shader->setUniform("u_light_type", (int)light->light_type);

	shader->setUniform("u_light_direction", light->model.rotateVector(Vector3(0, 0, -1)));

	shader->setUniform("u_light_exp", light->cone_exp);
	shader->setUniform("u_light_cosine_cutoff", (float)cos(light->cone_angle * DEG2RAD));

	if (light->shadowmap && light->cast_shadows) {
		shader->setUniform("u_light_cast_shadows_ml", light->cast_shadows);
		shader->setUniform("u_light_shadowmap_ml", light->shadowmap, 8);
		shader->setUniform("u_shadow_viewproj_ml", light->light_camera->viewprojection_matrix);
		shader->setUniform("u_light_shadowbias_ml", light->shadow_bias);
	}
	else {
		shader->setUniform("u_light_cast_shadows_ml", 0);
	}
}

// Shader Singlepass
void GTR::Renderer::uploadLightToShaderSinglepass(Shader* shader) {
	const int MAX_LIGHTS = 5;
	Vector3 light_position[MAX_LIGHTS];
	Vector3 light_color[MAX_LIGHTS];
	int light_type[MAX_LIGHTS];
	float light_max_distance[MAX_LIGHTS];
	Vector3 light_direction[MAX_LIGHTS];
	float light_exp[MAX_LIGHTS];
	float light_cosine_cutoff[MAX_LIGHTS];

	int light_cast_shadows[MAX_LIGHTS];
	Matrix44 shadow_viewproj[MAX_LIGHTS];
	float light_shadowbias[MAX_LIGHTS];
	Matrix44 empty;

	int num_lights = lights.size();
	int num_shadowmaps = 0;

	for (int i = 0; i < MAX_LIGHTS; ++i) {
		if (i < num_lights) {
			LightEntity* light = lights[i];

			light_position[i] = light->model * Vector3();
			light_color[i] = light->color * light->intensity;
			light_type[i] = light->light_type;
			light_max_distance[i] = light->max_distance;
			light_direction[i] = light->model.rotateVector(Vector3(0, 0, -1));
			light_exp[i] = light->cone_exp;
			light_cosine_cutoff[i] = cos(light->cone_angle * DEG2RAD);

			light_cast_shadows[i] = (int)light->cast_shadows;
			if (light->cast_shadows && light->light_camera) {
				shadow_viewproj[i] = light->light_camera->viewprojection_matrix;
				light_shadowbias[i] = light->shadow_bias;

				if (light->light_type == SPOT)
					shader->setUniform("u_light_shadowmap[0]", light->shadowmap, 11);
				else if (light->light_type == DIRECTIONAL)
					shader->setUniform("u_light_shadowmap[3]", light->shadowmap, 12);
			}
			else {
				shadow_viewproj[i] = empty;
				light_shadowbias[i] = NULL;
			}
		}
	}

	shader->setUniform3Array("u_light_color", (float*)&light_color, num_lights);
	shader->setUniform3Array("u_light_position", (float*)&light_position, num_lights);
	shader->setUniform3Array("u_light_direction", (float*)&light_direction, num_lights);
	shader->setUniform("u_num_lights", num_lights);

	shader->setUniform1Array("u_light_max_distance", (float*)&light_max_distance, num_lights);
	shader->setUniform1Array("u_light_type", (int*)&light_type, num_lights);
	shader->setUniform1Array("u_light_exp", (float*)&light_exp, num_lights);
	shader->setUniform1Array("u_light_cosine_cutoff", (float*)&light_cosine_cutoff, num_lights);

	shader->setUniform1Array("u_light_cast_shadows", (int*)&light_cast_shadows, num_lights);
	shader->setMatrix44Array("u_shadow_viewproj", shadow_viewproj, num_lights);
	shader->setUniform1Array("u_light_shadowbias", (float*)&light_shadowbias, num_lights);
}

// Shader Deferred
void GTR::Renderer::uploadLightToShaderDeferred(Shader* shader, Matrix44 inv_vp, int width, int height, Camera* camera) {
	shader->setUniform("u_gb0_texture", gbuffers_fbo->color_textures[0], 0);
	shader->setUniform("u_gb1_texture", gbuffers_fbo->color_textures[1], 1);
	shader->setUniform("u_gb2_texture", gbuffers_fbo->color_textures[2], 2);
	shader->setUniform("u_depth_texture", gbuffers_fbo->depth_texture, 3);
	shader->setUniform("u_ssao_texture", ssao_fbo->color_textures[0], 4);

	shader->setUniform("u_inverse_viewprojection", inv_vp);
	shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
	shader->setUniform("u_camera_pos", camera->eye);

	shader->setUniform("gamma_mode", (int)pipelineSpace);
	shader->setUniform("dynamic_range", (int)dynamicRange);
}

// Textures (emissive, occlusion, metallic...)
void GTR::Renderer::uploadUniformsAndTextures(Shader* shader, GTR::Material* material, Camera* camera, const Matrix44 model) {
	Texture* texture = NULL;
	Texture* emissive_texture = NULL;
	Texture* occlusion_texture = NULL;
	Texture* metallic_texture = NULL;
	Texture* normal_texture = NULL;
	GTR::Scene* scene = GTR::Scene::instance;

	if (texture == NULL)
		texture = Texture::getWhiteTexture(); //a 1x1 white texture

	//upload uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	float t = getTime();
	shader->setUniform("u_time", t);

	shader->setUniform("u_color", material->color);

	//Textures
	texture = material->color_texture.texture;
	Vector3 emissive_factor = material->emissive_factor;

	if (scene->emissive)
		emissive_texture = material->emissive_texture.texture;
	if (scene->occlussion) {
		occlusion_texture = material->occlusion_texture.texture;
		metallic_texture = material->metallic_roughness_texture.texture;
		shader->setUniform("u_metallic_factor", material->metallic_factor);
		shader->setUniform("u_roughness_factor", material->roughness_factor);
	}
	if (scene->normal)
		normal_texture = material->normal_texture.texture;

	if (texture)
		shader->setUniform("u_texture", texture, 0);
	if (emissive_texture) {
		shader->setUniform("u_emissive_texture", emissive_texture, 1);
		shader->setUniform("u_emissive_factor", emissive_factor);
	}
	else {
		shader->setUniform("u_emissive_factor", Vector3());
	}
	if (occlusion_texture)
		shader->setUniform("u_occlusion_texture", occlusion_texture, 2);
	else
		shader->setUniform("u_occlusion_texture", Texture::getWhiteTexture(), 2);
	if (metallic_texture)
		shader->setUniform("u_metallic_texture", metallic_texture, 3);
	else
		shader->setUniform("u_metallic_texture", Texture::getWhiteTexture(), 3);

	if (normal_texture) {
		shader->setUniform("has_normal", 1);
		shader->setUniform("u_normal_texture", normal_texture, 4);
	}
	else shader->setUniform("has_normal", 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == GTR::eAlphaMode::MASK ? material->alpha_cutoff : 0);
}