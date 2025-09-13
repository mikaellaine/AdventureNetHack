-- NetHack medusa medusa-2.lua	$NHDT-Date: 1652196027 2022/05/10 15:20:27 $  $NHDT-Branch: NetHack-3.7 $:$NHDT-Revision: 1.4 $
--	Copyright (c) 1989 by Jean-Christophe Collet
--	Copyright (c) 1990, 1991 by M. Stephenson
-- NetHack may be freely redistributed.  See license for details.
--
des.level_init({ style = "solidfill", fg = " " });

des.level_flags("mazelevel", "hardfloor", "stormy")

des.message("You hear a wind. You hear dragons.")

des.map([[
}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
}}}}}}}}}}}}}}}.}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
}}}}}}}}}}}}}}}.}}}}}}..}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
}}}}}}}}}}}}}}...}}}}......}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
|..}}....}}}...........|.............}}..................}}}}}}............
|...}..................|.............}....................}}}..............
|.....|.................|.............}....................}..............|
|----.|...---............|............}}..................................|
|...|.|.........|.........---..........}..................................|
|.....|.........|...........||.........}..................|...............|
|.|...|.........|...-.---...---..--...}}.....--....--........| |.|........|
|.---.....{....----...............-...}.......--...-...-|....| |.|...|....|
|.|............-- -..--.-..------.........--..-........------- |.--.------|
|+-----.........| |...|.|....|  --.......------...|....---------.....|....|
|...| --..------- |...|......|   ---...---    --..|...--......-...{.....-.|
|...|  ----       ------|....|     -----       -----.....----........|..|.|
-----                   ------                     -------  ---------------


]]);

-- Dungeon Description
des.region(selection.area(00,00,74,19),"lit")
des.region(selection.area(02,03,05,16),"unlit")
des.region({ region={61,03, 72,16}, lit=0, type="ordinary",irregular = 1 })
des.region(selection.area(71,08,72,11),"unlit")
-- Teleport: down to up stairs island, up to Medusa's island
des.teleport_region({ region = {02,03,05,16}, dir="down" })
des.teleport_region({ region = {61,03,72,16}, dir="up" })
-- Stairs
des.stair("up", 3,16)
-- Branch, not allowed on Medusa's island.
des.levregion({ type="branch", region = {01,00,79,20}, exclude = {59,01,73,17} })
-- Non diggable walls
des.non_diggable(selection.area(01,02,06,17))
des.non_diggable(selection.area(60,02,73,17))
-- Objects
des.object({ id = "statue", x=68,y=10,buc="uncursed",
                      montype="knight", historic=1, male=1,name="Perseus",
                      contents = function()
                         if percent(25) then
                            des.object({ id = "shield of reflection", buc="cursed", spe=0 })
                         end
                         if percent(75) then
                            des.object({ id = "levitation boots", spe=0 })
                         end
                         if percent(50) then
                            des.object({ id = "ring of levitation", buc="blessed", spe=2 })
                         end
                         if percent(50) then
                            des.object("sack")
                         end
                      end
});
des.object("boulder",04,04)
des.object("boulder",52,07)
des.object("boulder",42,08)
des.object("boulder",36,06)
-- Traps
des.monster("D",10,5)
des.monster("D",20,4)
des.monster("D",30,4)
des.monster("D",40,3)
des.monster("D",15,5)
des.monster("D",25,6)
des.monster("D",35,3)
des.monster("D",45,4)
des.monster({ id = "stone golem",x=64,y=18,asleep=1 })
des.monster({ id = "stone golem",x=65,y=19,asleep=1 })
des.monster("A",72,08)
des.monster("D")
des.monster("D")
des.monster("D")
des.monster("D")
des.monster("D")

