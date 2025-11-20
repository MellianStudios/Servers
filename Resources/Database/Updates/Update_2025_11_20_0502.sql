DROP TABLE IF EXISTS public.maps CASCADE;
CREATE TABLE public.maps
(
    id serial NOT NULL,
    flags integer NOT NULL DEFAULT 0,
    internal_name text NOT NULL DEFAULT '',
    name text NOT NULL DEFAULT '',
    type smallint NOT NULL DEFAULT 0,
    max_players smallint NOT NULL DEFAULT 0,
    PRIMARY KEY (id)
)

TABLESPACE pg_default;

ALTER TABLE IF EXISTS public.maps
    OWNER to postgres;