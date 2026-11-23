#include "GravityEngineSDL.h"
#include <algorithm>
GravityEngine_Core* geptr;

int snd_id;
int dth_snd_id;
int game_state = 1;
int last_score = -1;

class snake : public virtual GravityEngine_Object
{
    private:
        struct segment
        {
            int x;
            int y;
            color c;
        };
	public:
        int x = 0;
        int y = 0;
        int last_x = 0;
        int last_y = 0;
        color c = { {255, 255, 255}, {0, 0, 0} };
        int speed = 100;
        double xvel = 0;
        double yvel = 0;
        double to_xvel = 0;
        double to_yvel = 0;
        bool updated = false;
        int score = 0;
        int color_min = 10;
        int color_max = 255;
        bool increase_speed = false;
        std::vector<segment*> segments;
        snake() 
        { 
            int decide = (*geptr).RandRange(0, 3);
            if (decide == 0)
                xvel = 1, yvel = 0;
            if (decide == 1)
                xvel = -1, yvel = 0;
            if (decide == 2)
                xvel = 0, yvel = 1;
            if (decide == 3)
                xvel = 0, yvel = -1;
            to_xvel = xvel;
            to_yvel = yvel;
            x = floor((*geptr).GetCanvasW() / 2);
            y = floor((*geptr).GetCanvasH() / 2);
            c = { {(Uint8)(*geptr).RandRange(color_min, color_max),(Uint8)(*geptr).RandRange(color_min, color_max),(Uint8)(*geptr).RandRange(color_min, color_max)}, {0,0,0} };
            segments.insert(segments.end(), new segment(x, y, {{(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max)}, {0,0,0}}));
            segments.insert(segments.end(), new segment(x, y, {{(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max)}, {0,0,0}}));
            segments.insert(segments.end(), new segment(x, y, {{(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max)}, {0,0,0}}));
            segments.insert(segments.end(), new segment(x, y, {{(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max)}, {0,0,0}}));
        };
		~snake() {};
		void begin_step() 
        {
        };
		void step()
		{
            (*geptr).DrawTextString(0, (*geptr).GetCanvasH()-1, (*geptr).entity, std::to_string(score), {{255, 255, 255}, {0, 0, 0}});

            (*geptr).DrawChar(x, y, (*geptr).entity, '@');
			(*geptr).DrawSetColor(x,y,(*geptr).entity, c);
            for (auto s : segments)
            {
                (*geptr).DrawChar(s->x, s->y, (*geptr).entity, '@');
                (*geptr).DrawSetColor(s->x, s->y, (*geptr).entity, s->c);
            }

            if ((*geptr).GetKeyState(SDL_SCANCODE_RIGHT)) to_xvel = 1, to_yvel = 0;
            if ((*geptr).GetKeyState(SDL_SCANCODE_LEFT)) to_xvel = -1, to_yvel = 0;
            if ((*geptr).GetKeyState(SDL_SCANCODE_UP)) to_xvel = 0, to_yvel = -1;
            if ((*geptr).GetKeyState(SDL_SCANCODE_DOWN)) to_xvel = 0, to_yvel = 1;

            if ((*geptr).GetElapsedFrames() % speed == 0)
            {
                last_x = x;
                last_y = y;

                if (to_xvel == -1 && xvel == 1)
                    to_xvel = xvel;
                if (to_yvel == -1 && yvel == 1)
                    to_yvel = yvel;
                if (to_xvel == 1 && xvel == -1)
                    to_xvel = xvel;
                if (to_yvel == 1 && yvel == -1)
                    to_yvel = yvel;

                xvel = to_xvel;
                yvel = to_yvel;

                x += xvel;
                y += yvel;
                updated = true;
            }
            else
            {
                updated = false;
            }
		};
		void end_step() 
        {
            if (updated)
            {
                if (increase_speed && speed > 1) speed--, increase_speed = false;
                if (x < 0 || y < 0 || x >= (*geptr).GetCanvasW() || y >= (*geptr).GetCanvasH())
                {
                    Die();
                    return;
                }

                for (auto s : segments)
                {
                    int my_x = s->x;
                    int my_y = s->y;
                    s->x = last_x;
                    s->y = last_y;
                    last_x = my_x;
                    last_y = my_y;
                }

                for (auto s : segments)
                {
                    if (s->x == x && s->y == y)
                    {
                        Die();
                        return;
                    }
                }
            }
        };

        void Die()
        {
            (*geptr).RemoveObject(this);
            game_state = 3;
            (*geptr).PlaySoundOnChannel(dth_snd_id, 1);
        }

        void Grow()
        {
            segments.insert(segments.end(), new segment(segments[segments.size() - 1]->x, segments[segments.size() - 1]->y, {{(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max),(*geptr).RandRange(color_min, color_max)}, {0,0,0}}));
            increase_speed = true;
            score++;
            last_score = score;
        }
};

snake* snake_id;

class apple : public virtual GravityEngine_Object
{
private:
    int x;
    int y;
public:
    apple()
    {
        x = (Uint8)(*geptr).RandRange(0, (*geptr).GetCanvasW() - 1);
        y = (Uint8)(*geptr).RandRange(0, (*geptr).GetCanvasH() - 1);
    };
    ~apple() {};
    void step()
    {
        (*geptr).DrawChar(x, y, (*geptr).entity, 'O');
        (*geptr).DrawSetColor(x, y, (*geptr).entity, { {255,0,0}, {0,0,0} });

        if (snake_id != nullptr)
        {
            if (snake_id->x == x && snake_id->y == y)
            {
                (*geptr).PlaySoundOnChannel(snd_id, 0);
                x = (Uint8)(*geptr).RandRange(0, (*geptr).GetCanvasW() - 1);
                y = (Uint8)(*geptr).RandRange(0, (*geptr).GetCanvasH() - 1);
                snake_id->Grow();
            }
        }
    }
};

apple* apple_id;

// Master pre code
void GameInit()
{
    // Create a sound
    snd_id = (*geptr).AddSound(".\\pickupCoin.wav");
    dth_snd_id = (*geptr).AddSound(".\\death.wav");
}

// Master pre code
void PreGameLoop()
{
    if (game_state == 1)
    {
        std::string str = "Press space to start!";
        if (last_score != -1)
        {
            std::string scorestr = "Score: " + std::to_string(last_score);
            (*geptr).DrawTextString((*geptr).GetCanvasW() / 2 - floor(scorestr.length() / 2), (*geptr).GetCanvasH() / 2 + 1, (*geptr).entity, scorestr, { {255,255,255},{0,0,0} });
        }
        (*geptr).DrawTextString((*geptr).GetCanvasW() / 2 - floor(str.length()/2), (*geptr).GetCanvasH() / 2, (*geptr).entity, str, {{255,255,255},{0,0,0}});

        if ((*geptr).GetKeyState(SDL_SCANCODE_SPACE))
        {
            apple_id = new apple;
            snake_id = new snake;
            (*geptr).AddObject(snake_id);
            (*geptr).AddObject(apple_id);
            game_state = 2;
        }
    }
}

// Master post code
void PostGameLoop()
{
    if (game_state == 3)
    {
        delete[] snake_id;
        snake_id = nullptr;
        (*geptr).RemoveObject(apple_id);
        delete[] apple_id;
        apple_id = nullptr;
        game_state = 1;
    }
}

int main()
{
    // Init engine - 128x72 is generally the largest you can get and still maintain good performance
    GravityEngine_Core ge_inst = GravityEngine_Core("Snake", "com.example.snake", "1.0", 96/2, 54/2, 480, 1920, 1080, "./Ubuntu-B-1.ttf", 16);

    ge_inst.debug_mode = true; // Show debug overlay
    ge_inst.debug_complex = false; // Show all information
    geptr = &ge_inst; // Set the pointer to the console engine class

    // Start game loop
    ge_inst.Start(&GameInit, &PreGameLoop, &PostGameLoop);

    // Report success to host
    return 0;
}
