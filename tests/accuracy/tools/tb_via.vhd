-- Bus testbench for a 6522 model with the M6522 entity's ports (MikeJ's
-- m6522.vhd). Reads a script on stdin, one access per bus cycle:
--
--   w R V    write V to register R in the next cycle
--   r R      read register R in the next cycle; prints "R reg value cycle"
--   n K      K idle cycles
--   i        print "I irq cycle": the IRQ pin when the last cycle's data was taken
--
-- Values are hex. A cycle is PHI2 low then high, two model clocks each; data
-- is taken before the model clock on which PHI2 falls. The cycle count starts at 0
-- with the first scripted access.

library ieee;
   use ieee.std_logic_1164.all;
   use ieee.numeric_std.all;
library std;
   use std.textio.all;

entity tb_via is
end;

architecture sim of tb_via is
   signal clk     : std_logic := '0';
   signal p2      : std_logic := '0';
   signal rs      : std_logic_vector(3 downto 0) := x"0";
   signal din     : std_logic_vector(7 downto 0) := x"00";
   signal dout    : std_logic_vector(7 downto 0);
   signal dout_oe : std_logic;
   signal rw_l    : std_logic := '1';
   signal cs1     : std_logic := '0';
   signal irq_l   : std_logic;
   signal reset_l : std_logic := '0';
   signal irq_mid : std_logic := '1';
   signal pa_o, pa_oe, pb_o, pb_oe : std_logic_vector(7 downto 0);
   signal ca2_o, ca2_oe, cb1_o, cb1_oe, cb2_o, cb2_oe : std_logic;
begin
   dut : entity work.M6522
      port map (
         I_RS => rs, I_DATA => din, O_DATA => dout, O_DATA_OE_L => dout_oe,
         I_RW_L => rw_l, I_CS1 => cs1, I_CS2_L => '0', O_IRQ_L => irq_l,
         I_CA1 => '1', I_CA2 => '1', O_CA2 => ca2_o, O_CA2_OE_L => ca2_oe,
         I_PA => x"FF", O_PA => pa_o, O_PA_OE_L => pa_oe,
         I_CB1 => '1', O_CB1 => cb1_o, O_CB1_OE_L => cb1_oe,
         I_CB2 => '1', O_CB2 => cb2_o, O_CB2_OE_L => cb2_oe,
         I_PB => x"FF", O_PB => pb_o, O_PB_OE_L => pb_oe,
         I_P2_H => p2, RESET_L => reset_l, ENA_4 => '1', CLK => clk);

   p_run : process
      variable l, o   : line;
      variable cmd    : character;
      variable a, v   : std_logic_vector(7 downto 0);
      variable k      : integer;
      variable cycle  : integer := 0;
      variable good   : boolean;

      procedure tick is
      begin
         clk <= '1'; wait for 5 ns;
         clk <= '0'; wait for 5 ns;
      end procedure;

      -- One bus cycle; returns the data bus as the CPU latches it.
      procedure bus_cycle(sel : std_logic; reg : std_logic_vector(3 downto 0);
                          write : boolean; data : std_logic_vector(7 downto 0);
                          got : out std_logic_vector(7 downto 0)) is
      begin
         p2 <= '0';
         rs <= reg; din <= data; cs1 <= sel;
         if write then rw_l <= '0'; else rw_l <= '1'; end if;
         tick; tick;
         p2 <= '1';
         tick;
         -- The CPU latches data as PHI2 falls; the model's counters move on
         -- that same clock, so the bus is taken before it.
         got := dout;
         irq_mid <= irq_l;
         tick;
         cs1 <= '0'; rw_l <= '1';
      end procedure;

      procedure hex8(s : in string; x : out std_logic_vector(7 downto 0)) is
         variable n : integer := 0;
         variable c : character;
      begin
         for j in s'range loop
            c := s(j);
            if c >= '0' and c <= '9' then n := n * 16 + character'pos(c) - 48;
            elsif c >= 'A' and c <= 'F' then n := n * 16 + character'pos(c) - 55;
            elsif c >= 'a' and c <= 'f' then n := n * 16 + character'pos(c) - 87;
            end if;
         end loop;
         x := std_logic_vector(to_unsigned(n, 8));
      end procedure;

      variable word : string(1 to 8);
      variable wl   : integer;

      procedure next_word(variable ln : inout line) is
         variable c  : character;
         variable ok : boolean;
      begin
         wl := 0;
         loop
            read(ln, c, ok);
            exit when not ok;
            if c /= ' ' then
               wl := 1; word(1) := c;
               exit;
            end if;
         end loop;
         loop
            read(ln, c, ok);
            exit when not ok or c = ' ';
            wl := wl + 1; word(wl) := c;
         end loop;
      end procedure;

      function hexs(x : std_logic_vector(7 downto 0)) return string is
         constant h : string(1 to 16) := "0123456789ABCDEF";
         variable n : integer := to_integer(unsigned(x));
      begin
         return h(n / 16 + 1) & h(n mod 16 + 1);
      end function;

      variable dummy : std_logic_vector(7 downto 0);
   begin
      -- Reset over a few idle cycles, then let the phase counter settle.
      for j in 1 to 4 loop
         bus_cycle('0', x"0", false, x"00", dummy);
      end loop;
      reset_l <= '1';
      for j in 1 to 4 loop
         bus_cycle('0', x"0", false, x"00", dummy);
      end loop;

      while not endfile(input) loop
         readline(input, l);
         next_word(l);
         next when wl = 0;
         cmd := word(1);
         case cmd is
            when 'w' =>
               next_word(l); hex8(word(1 to wl), a);
               next_word(l); hex8(word(1 to wl), v);
               bus_cycle('1', a(3 downto 0), true, v, dummy);
               cycle := cycle + 1;
            when 'r' =>
               next_word(l); hex8(word(1 to wl), a);
               bus_cycle('1', a(3 downto 0), false, x"00", v);
               write(o, string'("R ") & hexs(a) & " " & hexs(v) & " " & integer'image(cycle));
               writeline(output, o);
               cycle := cycle + 1;
            when 'n' =>
               next_word(l);
               k := 0;
               for j in 1 to wl loop
                  k := k * 10 + character'pos(word(j)) - 48;
               end loop;
               for j in 1 to k loop
                  bus_cycle('0', x"0", false, x"00", dummy);
               end loop;
               cycle := cycle + k;
            when 'i' =>
               if irq_mid = '0' then
                  write(o, string'("I 1 ") & integer'image(cycle - 1));
               else
                  write(o, string'("I 0 ") & integer'image(cycle - 1));
               end if;
               writeline(output, o);
            when others => null;
         end case;
      end loop;
      wait;
   end process;
end;
