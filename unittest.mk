include makefile

OBJS =  db.o buf.o bufHash.o error.o page.o

catch_amalgamated.o:	deps/catch_amalgamated.cpp
	$(CXX) $(CXXFLAGS) -c deps/catch_amalgamated.cpp

unittest:	$(OBJS) catch_amalgamated.o unittest.cpp
		$(CXX) unittest.cpp -Ideps/ -o $@ $(OBJS) catch_amalgamated.o $(LDFLAGS)