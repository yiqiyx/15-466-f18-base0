#include "Game.hpp"

#include "gl_errors.hpp" //helper for dumpping OpenGL error messages
#include "read_chunk.hpp" //helper for reading a vector of structures from a file
#include "data_path.hpp" //helper to get paths relative to executable

#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <fstream>
#include <map>
#include <cstddef>
#include <random>

/*#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif*/

//helper defined later; throws if shader compilation fails:
static GLuint compile_shader(GLenum type, std::string const &source);

Game::Game() {
	{ //create an opengl program to perform sun/sky (well, directional+hemispherical) lighting:
		GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER,
			"#version 330\n"
			"uniform mat4 object_to_clip;\n"
			"uniform mat4x3 object_to_light;\n"
			"uniform mat3 normal_to_light;\n"
			"layout(location=0) in vec4 Position;\n" //note: layout keyword used to make sure that the location-0 attribute is always bound to something
			"in vec3 Normal;\n"
			"in vec4 Color;\n"
			"out vec3 position;\n"
			"out vec3 normal;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"	gl_Position = object_to_clip * Position;\n"
			"	position = object_to_light * Position;\n"
			"	normal = normal_to_light * Normal;\n"
			"	color = Color;\n"
			"}\n"
		);

		GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER,
			"#version 330\n"
			"uniform vec3 sun_direction;\n"
			"uniform vec3 sun_color;\n"
			"uniform vec3 sky_direction;\n"
			"uniform vec3 sky_color;\n"
			"in vec3 position;\n"
			"in vec3 normal;\n"
			"in vec4 color;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"	vec3 total_light = vec3(0.0, 0.0, 0.0);\n"
			"	vec3 n = normalize(normal);\n"
			"	{ //sky (hemisphere) light:\n"
			"		vec3 l = sky_direction;\n"
			"		float nl = 0.5 + 0.5 * dot(n,l);\n"
			"		total_light += nl * sky_color;\n"
			"	}\n"
			"	{ //sun (directional) light:\n"
			"		vec3 l = sun_direction;\n"
			"		float nl = max(0.0, dot(n,l));\n"
			"		total_light += nl * sun_color;\n"
			"	}\n"
			"	fragColor = vec4(color.rgb * total_light, color.a);\n"
			"}\n"
		);

		simple_shading.program = glCreateProgram();
		glAttachShader(simple_shading.program, vertex_shader);
		glAttachShader(simple_shading.program, fragment_shader);
		//shaders are reference counted so this makes sure they are freed after program is deleted:
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);

		//link the shader program and throw errors if linking fails:
		glLinkProgram(simple_shading.program);
		GLint link_status = GL_FALSE;
		glGetProgramiv(simple_shading.program, GL_LINK_STATUS, &link_status);
		if (link_status != GL_TRUE) {
			std::cerr << "Failed to link shader program." << std::endl;
			GLint info_log_length = 0;
			glGetProgramiv(simple_shading.program, GL_INFO_LOG_LENGTH, &info_log_length);
			std::vector< GLchar > info_log(info_log_length, 0);
			GLsizei length = 0;
			glGetProgramInfoLog(simple_shading.program, GLsizei(info_log.size()), &length, &info_log[0]);
			std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
			throw std::runtime_error("failed to link program");
		}
	}

	{ //read back uniform and attribute locations from the shader program:
		simple_shading.object_to_clip_mat4 = glGetUniformLocation(simple_shading.program, "object_to_clip");
		simple_shading.object_to_light_mat4x3 = glGetUniformLocation(simple_shading.program, "object_to_light");
		simple_shading.normal_to_light_mat3 = glGetUniformLocation(simple_shading.program, "normal_to_light");

		simple_shading.sun_direction_vec3 = glGetUniformLocation(simple_shading.program, "sun_direction");
		simple_shading.sun_color_vec3 = glGetUniformLocation(simple_shading.program, "sun_color");
		simple_shading.sky_direction_vec3 = glGetUniformLocation(simple_shading.program, "sky_direction");
		simple_shading.sky_color_vec3 = glGetUniformLocation(simple_shading.program, "sky_color");

		simple_shading.Position_vec4 = glGetAttribLocation(simple_shading.program, "Position");
		simple_shading.Normal_vec3 = glGetAttribLocation(simple_shading.program, "Normal");
		simple_shading.Color_vec4 = glGetAttribLocation(simple_shading.program, "Color");
	}

	struct Vertex {
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 28, "Vertex should be packed.");

	{ //load mesh data from a binary blob:
		// std::ifstream blob(data_path("meshes.blob"), std::ios::binary);
		std::ifstream blob(data_path("stickochet.blob"), std::ios::binary);
		//The blob will be made up of three chunks:
		// the first chunk will be vertex data (interleaved position/normal/color)
		// the second chunk will be characters
		// the third chunk will be an index, mapping a name (range of characters) to a mesh (range of vertex data)

		//read vertex data:
		std::vector< Vertex > vertices;
		read_chunk(blob, "dat0", &vertices);

		//read character data (for names):
		std::vector< char > names;
		read_chunk(blob, "str0", &names);

		//read index:
		struct IndexEntry {
			uint32_t name_begin;
			uint32_t name_end;
			uint32_t vertex_begin;
			uint32_t vertex_end;
		};
		static_assert(sizeof(IndexEntry) == 16, "IndexEntry should be packed.");

		std::vector< IndexEntry > index_entries;
		read_chunk(blob, "idx0", &index_entries);

		if (blob.peek() != EOF) {
			std::cerr << "WARNING: trailing data in meshes file." << std::endl;
		}

		//upload vertex data to the graphics card:
		glGenBuffers(1, &meshes_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * vertices.size(), vertices.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		//create map to store index entries:
		std::map< std::string, Mesh > index;
		for (IndexEntry const &e : index_entries) {
			if (e.name_begin > e.name_end || e.name_end > names.size()) {
				throw std::runtime_error("invalid name indices in index.");
			}
			if (e.vertex_begin > e.vertex_end || e.vertex_end > vertices.size()) {
				throw std::runtime_error("invalid vertex indices in index.");
			}
			Mesh mesh;
			mesh.first = e.vertex_begin;
			mesh.count = e.vertex_end - e.vertex_begin;
			auto ret = index.insert(std::make_pair(
				std::string(names.begin() + e.name_begin, names.begin() + e.name_end),
				mesh));
			if (!ret.second) {
				throw std::runtime_error("duplicate name in index.");
			}
		}

		//look up into index map to extract meshes:
		auto lookup = [&index](std::string const &name) -> Mesh {
			auto f = index.find(name);
			if (f == index.end()) {
				throw std::runtime_error("Mesh named '" + name + "' does not appear in index.");
			}
			return f->second;
		};
		checkpoint_mesh = lookup("Checkpoint");
		circle_mesh = lookup("Circle");
		floor_mesh = lookup("Floor");
		goal_mesh = lookup("Goal");
		plane_mesh = lookup("Plane");
		player_mesh = lookup("Player");
		wall_mesh = lookup("Wall");
	}

	{ //create vertex array object to hold the map from the mesh vertex buffer to shader program attributes:
		glGenVertexArrays(1, &meshes_for_simple_shading_vao);
		glBindVertexArray(meshes_for_simple_shading_vao);
		glBindBuffer(GL_ARRAY_BUFFER, meshes_vbo);
		//note that I'm specifying a 3-vector for a 4-vector attribute here, and this is okay to do:
		glVertexAttribPointer(simple_shading.Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
		glEnableVertexAttribArray(simple_shading.Position_vec4);
		if (simple_shading.Normal_vec3 != -1U) {
			glVertexAttribPointer(simple_shading.Normal_vec3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Normal));
			glEnableVertexAttribArray(simple_shading.Normal_vec3);
		}
		if (simple_shading.Color_vec4 != -1U) {
			glVertexAttribPointer(simple_shading.Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
			glEnableVertexAttribArray(simple_shading.Color_vec4);
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	GL_ERRORS();

	//----------------
	//set up game board with meshes and rolls:
	board_meshes.reserve(board_size.x * board_size.y);
	board_rotations.reserve(board_size.x * board_size.y);
	resetBoard();

	score = 0;
}

Game::~Game() {
	glDeleteVertexArrays(1, &meshes_for_simple_shading_vao);
	meshes_for_simple_shading_vao = -1U;

	glDeleteBuffers(1, &meshes_vbo);
	meshes_vbo = -1U;

	glDeleteProgram(simple_shading.program);
	simple_shading.program = -1U;

	GL_ERRORS();
}

bool Game::handle_event(SDL_Event const &evt, glm::uvec2 window_size) {
	//ignore any keys that are the result of automatic key repeat:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat) {
		return false;
	}
	//handle tracking the state of WSAD for roll control:
	if (evt.type == SDL_KEYDOWN || evt.type == SDL_KEYUP) {
		if (evt.key.keysym.scancode == SDL_SCANCODE_W) {
			controls.roll_up = (evt.type == SDL_KEYDOWN);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_S) {
			controls.roll_down = (evt.type == SDL_KEYDOWN);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_A) {
			controls.roll_left = (evt.type == SDL_KEYDOWN);
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_D) {
			controls.roll_right = (evt.type == SDL_KEYDOWN);
			return true;
		}
	}
	//move cursor on L/R/U/D press:
	if (evt.type == SDL_KEYDOWN && evt.key.repeat == 0) {
		if (evt.key.keysym.scancode == SDL_SCANCODE_LEFT) {
			Mesh nextMesh = getNextMesh(cursor.x-1, cursor.y);
			if (!isEqualMesh(nextMesh, plane_mesh)) {
				if (isEqualMesh(nextMesh, checkpoint_mesh)) {
					cursor.x -= 1;
					collectCheckPoint(cursor.x, cursor.y);
					score++;
				} else if (isEqualMesh(nextMesh, goal_mesh)) {
					cursor = glm::vec2(0,0);
					resetBoard();
					draw(glm::uvec2(640, 400));
				} else if (isEqualMesh(nextMesh, floor_mesh)) {
					cursor.x -= 1;
				}
			}
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_RIGHT) {
			Mesh nextMesh = getNextMesh(cursor.x+1, cursor.y);
			if (!isEqualMesh(nextMesh, plane_mesh)) {
				if (isEqualMesh(nextMesh, checkpoint_mesh)) {
					cursor.x += 1;
					collectCheckPoint(cursor.x, cursor.y);
					score++;
				} else if (isEqualMesh(nextMesh, goal_mesh)) {
					cursor = glm::vec2(0,0);
					resetBoard();
					draw(glm::uvec2(640, 400));
				} else if (isEqualMesh(nextMesh, floor_mesh)) {
					cursor.x += 1;
				}
			}
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_UP) {
			Mesh nextMesh = getNextMesh(cursor.x, cursor.y+1);
			if (!isEqualMesh(nextMesh, plane_mesh)) {
				if (isEqualMesh(nextMesh, checkpoint_mesh)) {
					cursor.y += 1;
					collectCheckPoint(cursor.x, cursor.y);
					score++;
				} else if (isEqualMesh(nextMesh, goal_mesh)) {
					cursor = glm::vec2(0,0);
					resetBoard();
					draw(glm::uvec2(640, 400));
				} else if (isEqualMesh(nextMesh, floor_mesh)) {
					cursor.y += 1;
				}
			}
			return true;
		} else if (evt.key.keysym.scancode == SDL_SCANCODE_DOWN) {
			Mesh nextMesh = getNextMesh(cursor.x, cursor.y-1);
			if (!isEqualMesh(nextMesh, plane_mesh)) {
				if (isEqualMesh(nextMesh, checkpoint_mesh)) {
					cursor.y -= 1;
					collectCheckPoint(cursor.x, cursor.y);
					score++;
				} else if (isEqualMesh(nextMesh, goal_mesh)) {
					cursor = glm::vec2(0,0);
					resetBoard();
					draw(glm::uvec2(640, 400));
				} else if (isEqualMesh(nextMesh, floor_mesh)) {
					cursor.y -= 1;
				}
			}
			return true;
		}
	}
	// press space to reset the game board
	if (evt.type == SDL_KEYDOWN && evt.key.repeat == 0) {
		if (evt.key.keysym.sym == SDLK_SPACE) {
			cursor = glm::vec2(0,0);
			resetBoard();
			draw(glm::uvec2(640, 400));
		}
	}
	return false;
}

Game::Mesh Game::getNextMesh(int x, int y) {
	if (x >= 0 && x < board_size.x && y >= 0 && y < board_size.y) {
		int idx = y * board_size.x + x;
		Mesh tmpMesh = *board_meshes[idx];
		return tmpMesh;
	}
	return plane_mesh;
}

void Game::collectCheckPoint(int x, int y) {
	int idx = y * board_size.x + x;
	board_meshes[idx] = &floor_mesh;
	draw(glm::uvec2(640, 400));
}

bool Game::isEqualMesh(Mesh a, Mesh b) {
	if (a.first == b.first && a.count == b.count) {
		return true;
	}
	return false;
}

void Game::update(float elapsed) {
	//if the roll keys are pressed, rotate everything on the same row or column as the cursor:
	glm::quat dr = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	float amt = elapsed * 1.0f;
	if (controls.roll_left) {
		dr = glm::angleAxis(amt, glm::vec3(0.0f, 1.0f, 0.0f)) * dr;
	}
	if (controls.roll_right) {
		dr = glm::angleAxis(-amt, glm::vec3(0.0f, 1.0f, 0.0f)) * dr;
	}
	if (controls.roll_up) {
		dr = glm::angleAxis(amt, glm::vec3(1.0f, 0.0f, 0.0f)) * dr;
	}
	if (controls.roll_down) {
		dr = glm::angleAxis(-amt, glm::vec3(1.0f, 0.0f, 0.0f)) * dr;
	}
	if (dr != glm::quat()) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			glm::quat &r = board_rotations[cursor.y * board_size.x + x];
			r = glm::normalize(dr * r);
		}
		for (uint32_t y = 0; y < board_size.y; ++y) {
			if (y != cursor.y) {
				glm::quat &r = board_rotations[y * board_size.x + cursor.x];
				r = glm::normalize(dr * r);
			}
		}
	}
}

/*void Game::drawScore() {
	std::string str = std::to_string(score);
	const char *charStr = str.c_str();
	glRasterPos3f(0, 0, 0);
	int i = 0;
	while (charStr[i] != 0) {
		glutBitmapCharacter(GLUT_BITMAP_TIMES_ROMAN_10, charStr[i]);
		i++;
	}
}*/

void Game::draw(glm::uvec2 drawable_size) {
	//Set up a transformation matrix to fit the board in the window:
	glm::mat4 world_to_clip;
	{
		float aspect = float(drawable_size.x) / float(drawable_size.y);

		//want scale such that board * scale fits in [-aspect,aspect]x[-1.0,1.0] screen box:
		float scale = glm::min(
			2.0f * aspect / float(board_size.x),
			2.0f / float(board_size.y)
		);

		//center of board will be placed at center of screen:
		glm::vec2 center = 0.5f * glm::vec2(board_size);

		//NOTE: glm matrices are specified in column-major order
		world_to_clip = glm::mat4(
			scale / aspect, 0.0f, 0.0f, 0.0f,
			0.0f, scale, 0.0f, 0.0f,
			0.0f, 0.0f,-1.0f, 0.0f,
			-(scale / aspect) * center.x, -scale * center.y, 0.0f, 1.0f
		);
	}

	//set up graphics pipeline to use data from the meshes and the simple shading program:
	glBindVertexArray(meshes_for_simple_shading_vao);
	glUseProgram(simple_shading.program);

	glUniform3fv(simple_shading.sun_color_vec3, 1, glm::value_ptr(glm::vec3(0.81f, 0.81f, 0.76f)));
	glUniform3fv(simple_shading.sun_direction_vec3, 1, glm::value_ptr(glm::normalize(glm::vec3(-0.2f, 0.2f, 1.0f))));
	glUniform3fv(simple_shading.sky_color_vec3, 1, glm::value_ptr(glm::vec3(0.2f, 0.2f, 0.3f)));
	glUniform3fv(simple_shading.sky_direction_vec3, 1, glm::value_ptr(glm::vec3(0.0f, 1.0f, 0.0f)));

	//helper function to draw a given mesh with a given transformation:
	auto draw_mesh = [&](Mesh const &mesh, glm::mat4 const &object_to_world) {
		//set up the matrix uniforms:
		if (simple_shading.object_to_clip_mat4 != -1U) {
			glm::mat4 object_to_clip = world_to_clip * object_to_world;
			glUniformMatrix4fv(simple_shading.object_to_clip_mat4, 1, GL_FALSE, glm::value_ptr(object_to_clip));
		}
		if (simple_shading.object_to_light_mat4x3 != -1U) {
			glUniformMatrix4x3fv(simple_shading.object_to_light_mat4x3, 1, GL_FALSE, glm::value_ptr(object_to_world));
		}
		if (simple_shading.normal_to_light_mat3 != -1U) {
			//NOTE: if there isn't any non-uniform scaling in the object_to_world matrix, then the inverse transpose is the matrix itself, and computing it wastes some CPU time:
			glm::mat3 normal_to_world = glm::inverse(glm::transpose(glm::mat3(object_to_world)));
			glUniformMatrix3fv(simple_shading.normal_to_light_mat3, 1, GL_FALSE, glm::value_ptr(normal_to_world));
		}

		//draw the mesh:
		glDrawArrays(GL_TRIANGLES, mesh.first, mesh.count);
	};

	for (uint32_t y = 0; y < board_size.y; ++y) {
		for (uint32_t x = 0; x < board_size.x; ++x) {
			draw_mesh(floor_mesh,
				glm::mat4(
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					x+0.5f, y+0.5f,-0.5f, 1.0f
				)
			);
			draw_mesh(*board_meshes[y*board_size.x+x],
				glm::mat4(
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					x+0.5f, y+0.5f, 0.0f, 1.0f
				)
				* glm::mat4_cast(board_rotations[y*board_size.x+x])
			);
		}
	}
	draw_mesh(player_mesh,
		glm::mat4(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			cursor.x+0.5f, cursor.y+0.5f, 0.0f, 1.0f
		)
	);

	//drawScore();

	glUseProgram(0);

	GL_ERRORS();
}



//create and return an OpenGL vertex shader from source:
static GLuint compile_shader(GLenum type, std::string const &source) {
	GLuint shader = glCreateShader(type);
	GLchar const *str = source.c_str();
	GLint length = GLint(source.size());
	glShaderSource(shader, 1, &str, &length);
	glCompileShader(shader);
	GLint compile_status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
	if (compile_status != GL_TRUE) {
		std::cerr << "Failed to compile shader." << std::endl;
		GLint info_log_length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);
		std::vector< GLchar > info_log(info_log_length, 0);
		GLsizei length = 0;
		glGetShaderInfoLog(shader, GLsizei(info_log.size()), &length, &info_log[0]);
		std::cerr << "Info log: " << std::string(info_log.begin(), info_log.begin() + length);
		glDeleteShader(shader);
		throw std::runtime_error("Failed to compile shader.");
	}
	return shader;
}

// reset the board
void Game::resetBoard() {
	board_meshes.clear();
	board_rotations.clear();
	std::mt19937 mt(0xbead1234);

	std::vector< Mesh const * > meshes{ &checkpoint_mesh, &wall_mesh, &circle_mesh, &floor_mesh };

	for (uint32_t i = 0; i < board_size.x * board_size.y; ++i) {
		board_meshes.emplace_back(meshes[(std::rand()+1)%meshes.size()]);
		board_rotations.emplace_back(glm::quat());
	}

	// clear the first location for player
	board_meshes[0] = &floor_mesh;

	// place a goal
	while (true) {
		int idx = mt()%board_meshes.size();
		if (idx != 0) {
			board_meshes[idx] = &goal_mesh;
			break;
		}
	}
}
