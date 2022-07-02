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
	probes_texture = NULL;
	irradiance_fbo = NULL;
	volumetric_fbo = NULL;

	reflection_fbo = new FBO();
	reflection_fbo->create(Application::instance->window_width, Application::instance->window_height);
	reflection_probe_fbo = new FBO();

	multilight = true;
	show_gbuffers = false;
	show_ssao = false;
	show_probes = false;
	show_reflection_probes = false;
	show_probes_texture = false;
	show_irradiance = false;
	show_reflections = false;
	show_decal = false;
	is_rendering_reflections = false;
	show_chrab_lensdist = false;
	show_motblur = false;
	show_antial = false;
	show_DoF = false;
	show_volumetric = false;

	random_points = generateSpherePoints(64, 1, true);
	skybox = CubemapFromHDRE("data/pisa.hdre");

	cloned_depth_texture = NULL;
	decal_fbo = NULL;
	cube.createCube();
	postFX_textureA = NULL;
	postFX_textureB = NULL;
	postFX_textureC = NULL;
	postFX_textureD = NULL;

	blur = NULL;
	directional = NULL;
	deb_fac = 1.0;
	minDist = 1.0;
	maxDist = 300.0;
	air_density = 1.0;
	intensity_factor = 1.0;
	contrast = 1.0;
	threshold = 1.0;

	loadProbes();

	// MIRAAAAAAR
	//create the probe
	sReflectionProbe* probe = new sReflectionProbe;
	//set it up
	probe->pos.set(90, 56, -72);
	probe->cubemap = new Texture();
	probe->cubemap->createCubemap(512, 512, NULL, GL_RGB, GL_UNSIGNED_INT, true);
	//add it to the list
	reflection_probes.push_back(probe);

	probe = new sReflectionProbe;
	probe->pos.set(90, 56, 128);
	probe->cubemap = new Texture();
	probe->cubemap->createCubemap(512, 512, NULL, GL_RGB, GL_UNSIGNED_INT, true);
	reflection_probes.push_back(probe);

	probe = new sReflectionProbe;
	probe->pos.set(90, 56, 328);
	probe->cubemap = new Texture();
	probe->cubemap->createCubemap(512, 512, NULL, GL_RGB, GL_UNSIGNED_INT, true);
	reflection_probes.push_back(probe);
}

void GTR::Renderer::generateSkybox(Camera* camera) {
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);
	Shader* shader = Shader::Get("skybox");
	Matrix44 model;
	model.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);

	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform("u_texture", skybox, 0);
	mesh->render(GL_TRIANGLES);
	shader->disable();

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
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

	generateSkybox(camera);

	for (vector<GTR::RenderCall>::iterator rc = render_calls.begin(); rc != render_calls.end(); ++rc) {
		if (camera->testBoxInFrustum(rc->world_bounding.center, rc->world_bounding.halfsize))
			renderMeshWithMaterialAndLighting(rc->model, rc->mesh, rc->material, camera);
	}

	if (show_probes)
		for (int i = 0; i < probes.size(); i++)
			renderProbe(probes[i].pos, 2, probes[i].sh.coeffs[0].v);
	if (show_reflection_probes)
		renderReflectionProbes(scene, camera);
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
	if (!decal_fbo) {
		decal_fbo = new FBO();
		decal_fbo->create(width, height,
			3,			//one texture
			GL_RGBA,			//three channels
			GL_UNSIGNED_BYTE,	//1 byte
			true);		//add depth_texture
	}
	if (!postFX_textureA) {
		postFX_textureA = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}
	if (!postFX_textureB) {
		postFX_textureB = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}
	if (!postFX_textureC) {
		postFX_textureC = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}
	if (!postFX_textureD) {
		postFX_textureD = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}
	if (!blur) {
		blur = new Texture(width, height, GL_RGB, GL_FLOAT, false);
	}

	Mesh* quad = Mesh::getQuad();
	Mesh* sphere = Mesh::Get("data/meshes/sphere.obj", false);
	Matrix44 inv_view = camera->view_matrix;
	inv_view.inverse();
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

	gbuffers_fbo->color_textures[0]->copyTo(decal_fbo->color_textures[0]);
	gbuffers_fbo->color_textures[1]->copyTo(decal_fbo->color_textures[1]);
	gbuffers_fbo->color_textures[2]->copyTo(decal_fbo->color_textures[2]);

	decal_fbo->bind();
	gbuffers_fbo->depth_texture->copyTo(NULL);
	decal_fbo->unbind();

	if (decals.size() && show_decal) {
		gbuffers_fbo->bind();

		Shader* shader = Shader::Get("decal");
		shader->enable();
		shader->setUniform("u_depth_texture", decal_fbo->depth_texture, 6);
		shader->setUniform("u_inverse_viewprojection", inv_vp);
		shader->setUniform("u_iRes", Vector2(1.0 / (float)width, 1.0 / (float)height));
		shader->setUniform("u_viewprojection", camera->viewprojection_matrix);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glColorMask(true, true, true, false);

		for (int i = 0; i < decals.size(); i++) {
			DecalEntity* decal = decals[i];
			shader->setUniform("u_model", decal->model);
			Matrix44 inv_decal_model = decal->model;
			inv_decal_model.inverse();
			shader->setUniform("u_imodel", inv_decal_model);
			Texture* decal_texture = Texture::Get(decal->texture.c_str());
			if (!decal_texture) continue;
			shader->setUniform("u_texture", decal_texture, 7);
			cube.render(GL_TRIANGLES);
		}

		glColorMask(true, true, true, true);
		glDisable(GL_BLEND);
		gbuffers_fbo->unbind();
	}

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

	uploadLightToShaderMultipass(directional, shader);

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

	if (probes_texture && show_irradiance) {
		shader = Shader::Get("irradiance");
		shader->enable();
		uploadLightToShaderDeferred(shader, inv_vp, width, height, camera);
		shader->setUniform("u_inv_view_matrix", inv_view);
		shader->setUniform("u_probes_texture", probes_texture, 5);
		shader->setUniform("u_irr_start", start_irr);
		shader->setUniform("u_irr_end", end_irr);
		shader->setUniform("u_irr_dim", dim_irr);
		shader->setUniform("u_irr_normal_distance", 0.1f);
		shader->setUniform("u_irr_delta", delta);
		shader->setUniform("u_num_probes", probes_texture->height);

		quad->render(GL_TRIANGLES);
	}

	// To enable the z-buffer so the grid does not appear over the objects
	glEnable(GL_DEPTH_TEST);

	// Render alphanodes in forward mode
	for (vector<GTR::RenderCall>::iterator rc = render_calls.begin(); rc != render_calls.end(); ++rc) {
		if (camera->testBoxInFrustum(rc->world_bounding.center, rc->world_bounding.halfsize) && rc->material->alpha_mode == GTR::eAlphaMode::BLEND) {
			renderMeshWithMaterialAndLighting(rc->model, rc->mesh, rc->material, camera);
		}
	}

	illumination_fbo->unbind();

	applyfx(illumination_fbo->color_textures[0], gbuffers_fbo->depth_texture, camera);

	if (show_volumetric) {
		if (!volumetric_fbo) {
			volumetric_fbo = new FBO();
			volumetric_fbo->create(width, height, 1, GL_RGBA);
		}

		volumetric_fbo->bind();
		shader = Shader::Get("volumetric");
		shader->enable();
		uploadLightToShaderDeferred(shader, inv_vp, width, height, camera);
		uploadLightToShaderSinglepass(shader);
		shader->setUniform("u_air_density", air_density * 0.001f);
		quad->render(GL_TRIANGLES);
		volumetric_fbo->unbind();
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		volumetric_fbo->color_textures[0]->toViewport();
	}

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
	decals.clear();

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
			if (light->light_type == DIRECTIONAL)
				directional = light;
		}

		//is a prefab!
		if (ent->entity_type == PREFAB)
		{
			PrefabEntity* pent = (GTR::PrefabEntity*)ent;
			if (pent->prefab) {
				renderPrefab(ent->model, pent->prefab, camera);
			}
		}

		//is a decal!
		if (ent->entity_type == DECALL)
		{
			DecalEntity* decal = (GTR::DecalEntity*)ent;
			decals.push_back(decal);
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
	// Forward
	if (pipeline == FORWARD) {
		if (show_reflections) {
			reflection_fbo->bind();
			Camera flipped_camera;
			flipped_camera.lookAt(camera->eye * Vector3(1, -1, 1), camera->center * Vector3(1, -1, 1), Vector3(0, -1, 0));
			flipped_camera.setPerspective(camera->fov, camera->aspect, camera->near_plane, camera->far_plane);
			flipped_camera.enable();
			is_rendering_reflections = true;
			renderForward(&flipped_camera, scene);
			is_rendering_reflections = false;
			reflection_fbo->unbind();
			camera->enable();
		}
		renderForward(camera, scene);
	}
	// Deferred
	else if (pipeline == DEFERRED)
		renderDeferred(camera, scene);

	if (probes_texture && show_probes_texture)
		probes_texture->toViewport();
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
void GTR::Renderer::uploadUniformsAndTextures(Shader* shader, GTR::Material* material, Camera* camera, Matrix44 model) {
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
		shader->setUniform("u_emissive_factor", emissive_factor);
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

	if (!is_rendering_reflections && reflection_probes.size() > 0) {
		sReflectionProbe* rprobe = reflection_probes[0];
		Vector3 modelPos = model.getTranslation();
		for (int i = 1; i < reflection_probes.size(); i++) {
			sReflectionProbe* newrprobe = reflection_probes[i];
			if (newrprobe->pos.distance(modelPos) < rprobe->pos.distance(modelPos)) {
				rprobe = newrprobe;
			}
		}
		if (!rprobe->cubemap)
			shader->setUniform("is_reflection", 0);
		else {
			shader->setUniform("is_reflection", show_reflections);
			shader->setUniform("u_reflection_texture", rprobe->cubemap, 10);
		}
	}
	else {
		shader->setUniform("is_reflection", 0);
	}
}

// Generate Probes
void GTR::Renderer::generateProbe(GTR::Scene* scene) {
	//define the corners of the axis aligned grid
	//this can be done using the boundings of our scene
	start_irr.set(-300, 5, -400);
	end_irr.set(300, 150, 400);

	//define how many probes you want per dimension
	dim_irr.set(10, 6, 10);

	//compute the vector from one corner to the other
	delta = (end_irr - start_irr);

	probes.clear();

	//and scale it down according to the subdivisions
	//we substract one to be sure the last probe is at end pos
	delta.x /= (dim_irr.x - 1);
	delta.y /= (dim_irr.y - 1);
	delta.z /= (dim_irr.z - 1);
	for (int z = 0; z < dim_irr.z; ++z) {
		for (int y = 0; y < dim_irr.y; ++y) {
			for (int x = 0; x < dim_irr.x; ++x) {
				sProbe p;
				p.local.set(x, y, z);

				//index in the linear array
				p.index = x + y * dim_irr.x + z * dim_irr.x * dim_irr.y;

				//and its position
				p.pos = start_irr + delta * Vector3(x, y, z);
				probes.push_back(p);
			}
		}
	}

	for (int iP = 0; iP < probes.size(); ++iP) {
		int probe_index = iP;
		captureProbe(probes[iP], scene);
		std::cout << "Generating probe number " << iP << " de " << probes.size() << std::endl;
	}

	if (probes_texture != NULL) delete probes_texture;

	cout << "DONE" << endl;

	uploadProbesToGPU();

	// saveIrradianceToDisk ---------------------------------

	//fill header structure
	sIrrHeader header;

	header.start = start_irr;
	header.end = end_irr;
	header.dims = dim_irr;
	header.delta = delta;
	header.num_probes = dim_irr.x * dim_irr.y * dim_irr.z;

	//write to file header and probes data
	FILE* f = fopen("irradiance.bin", "wb");
	fwrite(&header, sizeof(header), 1, f);
	fwrite(&(probes[0]), sizeof(sProbe), probes.size(), f);
	fclose(f);
}

bool Renderer::loadProbes() {
	//load probes info from disk
	FILE* f = fopen("irradiance.bin", "rb");
	if (!f)
		return false;

	//read header
	sIrrHeader header;
	fread(&header, sizeof(header), 1, f);

	//copy info from header to our local vars
	start_irr = header.start;
	end_irr = header.end;
	dim_irr = header.dims;
	delta = header.delta;
	int num_probes = header.num_probes;

	//allocate space for the probes
	probes.resize(num_probes);

	//read from disk directly to our probes container in memory
	fread(&probes[0], sizeof(sProbe), probes.size(), f);
	fclose(f);

	uploadProbesToGPU();
}

void GTR::Renderer::uploadProbesToGPU() {
	//create the texture to store the probes (do this ONCE!!!)
	if (probes_texture == NULL) {
		delete probes_texture;
	}
	probes_texture = new Texture(
		9, //9 coefficients per probe
		probes.size(), //as many rows as probes
		GL_RGB, //3 channels per coefficient
		GL_FLOAT); //they require a high range

	SphericalHarmonics* sh_data = NULL;
	sh_data = new SphericalHarmonics[dim_irr.x * dim_irr.y * dim_irr.z];

	//here we fill the data of the array with our probes in x,y,z order
	for (int i = 0; i < probes.size(); ++i)
		sh_data[i] = probes[i].sh;

	//now upload the data to the GPU as a texture
	probes_texture->upload(GL_RGB, GL_FLOAT, false, (uint8*)sh_data);

	//disable any texture filtering when reading
	probes_texture->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	//always free memory after allocating it!!!
	delete[] sh_data;
}

void GTR::Renderer::renderProbe(Vector3 pos, float size, float* coeffs) {
	Camera* camera = Camera::current;
	Shader* shader = Shader::Get("probe");
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);

	glEnable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	Matrix44 model;
	model.setTranslation(pos.x, pos.y, pos.z);
	model.scale(size, size, size);

	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_model", model);
	shader->setUniform3Array("u_coeffs", coeffs, 9);

	mesh->render(GL_TRIANGLES);
}

void GTR::Renderer::captureProbe(sProbe& probe, GTR::Scene* scene) {
	FloatImage images[6]; //here we will store the six views
	Camera camera;

	if (irradiance_fbo == NULL) {
		irradiance_fbo = new FBO();
		irradiance_fbo->create(64, 64, 1, GL_RGB, GL_FLOAT);
	}
	//set the fov to 90 and the aspect to 1
	camera.setPerspective(90, 1, 0.1, 1000);

	for (int i = 0; i < 6; ++i) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		Vector3 eye = probe.pos;
		Vector3 front = cubemapFaceNormals[i][2];
		Vector3 center = probe.pos + front;
		Vector3 up = cubemapFaceNormals[i][1];
		camera.lookAt(eye, center, up);
		camera.enable();

		//render the scene from this point of view
		irradiance_fbo->bind();
		renderForward(&camera, scene);
		irradiance_fbo->unbind();

		//read the pixels back and store in a FloatImage
		images[i].fromTexture(irradiance_fbo->color_textures[0]);
	}

	//compute the coefficients given the six images
	probe.sh = computeSH(images);
}

void GTR::Renderer::renderReflectionProbes(GTR::Scene* scene, Camera* camera) {
	Mesh* mesh = Mesh::Get("data/meshes/sphere.obj", false);
	Shader* shader = Shader::Get("reflection_probe");
	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	Matrix44 model;
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	for (int i = 0; i < reflection_probes.size(); i++) {
		sReflectionProbe* probe = reflection_probes[i];
		if (!probe->cubemap)
			continue;
		model.setTranslation(probe->pos.x, probe->pos.y, probe->pos.z);
		shader->setUniform("u_model", model);
		shader->setUniform("u_texture", probe->cubemap, 0);

		mesh->render(GL_TRIANGLES);
	}
	shader->disable();
}

void GTR::Renderer::updateReflectionProbes(GTR::Scene* scene) {
	for (int i = 0; i < reflection_probes.size(); i++) {
		sReflectionProbe* probe = reflection_probes[i];
		if (!probe->cubemap) {
			probe->cubemap = new Texture();
			probe->cubemap->createCubemap(512, 512, NULL, GL_RGB, GL_UNSIGNED_INT, true);
		}
		captureReflectionProbe(scene, probe->cubemap, probe->pos);
	}
}

void GTR::Renderer::captureReflectionProbe(GTR::Scene* scene, Texture* tex, Vector3 pos) {
	Camera camera;

	for (int i = 0; i < 6; i++) {
		reflection_probe_fbo->setTexture(tex, i);
		camera.setPerspective(90, 1, 0.1, 1000);
		Vector3 eye = pos;
		Vector3 center = pos + cubemapFaceNormals[i][2];
		Vector3 up = cubemapFaceNormals[i][1];
		camera.lookAt(eye, center, up);
		camera.enable();
		reflection_probe_fbo->bind();
		is_rendering_reflections = true;
		renderForward(&camera, scene);
		is_rendering_reflections = false;
		reflection_probe_fbo->unbind();
	}
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	tex->generateMipmaps();
}

void GTR::Renderer::applyfx(Texture* color, Texture* depth, Camera* camera) {
	Texture* current_texture = color;
	int width = Application::instance->window_width;
	int height = Application::instance->window_height;
	Matrix44 inv_vp = camera->viewprojection_matrix;
	inv_vp.inverse();

	//Depth of Field
	if (show_DoF) {
		//Blur
		FBO* blur_fbo;
		Shader* blur_shader;
		for (int i = 0; i < 16; i++) {
			blur_fbo = Texture::getGlobalFBO(postFX_textureA);
			blur_fbo->bind();
			blur_shader = Shader::Get("blurredof");
			blur_shader->enable();
			blur_shader->setUniform("u_offset", vec2(pow(1.0f, i) / current_texture->width, 0.0) * deb_fac);
			blur_shader->setUniform("u_intensity", 1.0f);
			current_texture->toViewport(blur_shader);
			blur_shader->disable();
			blur_fbo->unbind();

			blur_fbo = Texture::getGlobalFBO(blur);
			blur_fbo->bind();
			blur_shader = Shader::Get("blurredof");
			blur_shader->enable();
			blur_shader->setUniform("u_offset", vec2(0.0, pow(1.0f, i) / current_texture->height) * deb_fac);
			blur_shader->setUniform("u_intensity", 1.0f);
			postFX_textureA->toViewport(blur_shader);
			blur_shader->disable();
			blur_fbo->unbind();
			current_texture = blur;
		}
		current_texture = color;

		//Depth of Field
		FBO* dof_fbo = Texture::getGlobalFBO(postFX_textureA);
		dof_fbo->bind();
		Shader* dof_shader = Shader::Get("depth_of_field");
		dof_shader->enable();
		dof_shader->setUniform("u_outoffocus_texture", blur, 2);
		dof_shader->setUniform("u_depth_texture", depth, 3);
		dof_shader->setUniform("u_inverse_viewprojection", inv_vp);
		dof_shader->setUniform("minDistance", minDist);
		dof_shader->setUniform("maxDistance", maxDist);
		current_texture->toViewport(dof_shader);
		dof_shader->disable();
		dof_fbo->unbind();
		current_texture = postFX_textureA;
		swap(postFX_textureA, postFX_textureB);
	}

	//Chromatic aberration and lens distortion
	if (show_chrab_lensdist) {
		FBO* ch_fbo = Texture::getGlobalFBO(postFX_textureA);
		ch_fbo->bind();
		Shader* ch_shader = Shader::Get("chrlen");
		ch_shader->enable();
		ch_shader->setUniform("resolution", Vector2((float)width, (float)height));
		current_texture->toViewport(ch_shader);
		ch_shader->disable();
		ch_fbo->unbind();
		current_texture = postFX_textureA;
		swap(postFX_textureA, postFX_textureB);
	}

	//Motion blur
	if (show_motblur) {
		FBO* mot_fbo = Texture::getGlobalFBO(postFX_textureA);
		mot_fbo->bind();
		Shader* mot_shader = Shader::Get("motionblur");
		mot_shader->enable();
		mot_shader->setUniform("u_depth_texture", depth, 1);
		mot_shader->setUniform("u_inverse_viewprojection", inv_vp);
		mot_shader->setUniform("u_viewprojection_old", viewproj_old);
		current_texture->toViewport(mot_shader);
		mot_shader->disable();
		mot_fbo->unbind();
		current_texture = postFX_textureA;
		swap(postFX_textureA, postFX_textureB);
		viewproj_old = camera->viewprojection_matrix;
	}

	//Antialiasing
	if (show_antial) {
		FBO* al_fbo = Texture::getGlobalFBO(postFX_textureA);
		al_fbo->bind();
		Shader* al_shader = Shader::Get("antialiasing");
		al_shader->enable();
		al_shader->setUniform("u_viewportSize", Vector2((float)width, (float)height));
		al_shader->setUniform("u_iViewportSize", Vector2(1.0 / (float)width, 1.0 / (float)height));
		current_texture->toViewport(al_shader);
		al_shader->disable();
		al_fbo->unbind();
		current_texture = postFX_textureA;
		swap(postFX_textureA, postFX_textureB);
	}

	//Tonemapper
	glDisable(GL_BLEND);
	Shader* shader_tm = Shader::Get("tonemapper");
	shader_tm->enable();
	current_texture->toViewport(shader_tm);
}