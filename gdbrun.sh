make -j4
cp .libs/libmergeodf.so.0.0.0 libmergeodf.so
cp .libs/libtbl2sc.so.0.0.0 libtbl2sc.so
./loolwsd --o:num_prespawn_children=1
