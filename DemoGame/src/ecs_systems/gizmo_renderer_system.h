#pragma once
#include "ecs/i_simple_system.h"

#include "resource_handlers/gizmo_resource_handler.h"

#include "SDL_image.h"

class GizmoRendererSystem : public ECS::ISimpleSystem
{
	public:
		GizmoRendererSystem( SDL_Renderer* renderer );

		GizmoResourceHandler& GetGizmoResourceHandler() { return _gizmoResourceHandler; }

		void Execute( ECS::EntityContainer& entity_container, float32 elapsed_time ) override;

		void AllocateGizmoRendererComponent( ECS::GameEntity& entity );
		void DeallocateGizmoRendererComponent( ECS::GameEntity& entity );

	private:
		SDL_Renderer* _renderer;
		GizmoResourceHandler _gizmoResourceHandler;
};
